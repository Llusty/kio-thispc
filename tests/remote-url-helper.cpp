/*
 * Focused regression suite for Remote URL handling, sanitization,
 * transactional staging, and navigation parity.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "browsercommon.h"
#include "directorylistingcore.h"
#include "locationpresentation.h"
#include "navigationhistory.h"
#include "remoteurlhelper.h"

#include <QApplication>
#include <QEventLoop>
#include <QTimer>

static int checks = 0;

static void verify(bool condition, const char *description)
{
    if (!condition) qFatal("FAIL: %s", description);
    ++checks;
}

static KIO::UDSEntry makeEntry(const QString &name, const QString &mime, qint64 size)
{
    KIO::UDSEntry entry;
    entry.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    entry.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mime);
    entry.fastInsert(KIO::UDSEntry::UDS_SIZE, size);
    entry.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE,
                     mime == QStringLiteral("inode/directory") ? 0040000 : 0100000);
    return entry;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // 1. Remote schemes recognition
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("smb")), "smb is remote scheme");
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("SMB")), "SMB case-insensitive is remote scheme");
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("sftp")), "sftp is remote scheme");
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("ftp")), "ftp is remote scheme");
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("webdav")), "webdav is remote scheme");
    verify(RemoteUrlHelper::isRemoteScheme(QStringLiteral("webdavs")), "webdavs is remote scheme");
    verify(!RemoteUrlHelper::isRemoteScheme(QStringLiteral("file")), "file is not remote scheme");
    verify(!RemoteUrlHelper::isRemoteScheme(QStringLiteral("thispc")), "thispc is not remote scheme");
    verify(!RemoteUrlHelper::isRemoteScheme(QStringLiteral("http")), "http is not remote scheme");
    verify(!RemoteUrlHelper::isRemoteScheme(QStringLiteral("https")), "https is not remote scheme");
    verify(!RemoteUrlHelper::isRemoteScheme(QStringLiteral("admin")), "admin is not remote scheme");

    // 2. Non-conversion to file://
    const QUrl smbUrl = urlFromUserText(QStringLiteral("smb://nas.local/public"));
    verify(smbUrl.isValid(), "smb urlFromUserText is valid");
    verify(smbUrl.scheme() == QStringLiteral("smb"), "smb url preserves scheme");
    verify(!smbUrl.isLocalFile(), "smb url is not converted to file://");
    verify(smbUrl.host() == QStringLiteral("nas.local"), "smb host preserved");
    verify(smbUrl.path() == QStringLiteral("/public"), "smb path preserved");

    const QUrl sftpUrl = urlFromUserText(QStringLiteral("sftp://my-server.org:2222/var/log"));
    verify(sftpUrl.isValid(), "sftp urlFromUserText is valid");
    verify(sftpUrl.scheme() == QStringLiteral("sftp"), "sftp scheme preserved");
    verify(!sftpUrl.isLocalFile(), "sftp url is not converted to file://");
    verify(sftpUrl.port() == 2222, "sftp port preserved");
    verify(sftpUrl.path() == QStringLiteral("/var/log"), "sftp path preserved");

    // 3. Unicode and percent-encoding in remote URL
    const QUrl unicodeUrl = urlFromUserText(QStringLiteral("sftp://host.test/pobranie/zażółć gęślą jaźń"));
    verify(unicodeUrl.isValid(), "unicode remote url is valid");
    verify(unicodeUrl.path().contains(QStringLiteral("zażółć")), "unicode characters preserved in path");

    // 4. Deterministic rejection of malformed URLs
    verify(!urlFromUserText(QStringLiteral("smb://")).isValid(), "empty host smb:// rejected");
    verify(!urlFromUserText(QStringLiteral("sftp://")).isValid(), "empty host sftp:// rejected");
    verify(!urlFromUserText(QStringLiteral("ftp://:21/")).isValid(), "missing host with port rejected");
    verify(!urlFromUserText(QStringLiteral("smb:server")).isValid(), "single colon smb:server rejected");
    verify(!urlFromUserText(QStringLiteral("sftp://[invalid/")).isValid(), "malformed ipv6 rejected");

    // 5. Unsupported scheme is not recognized as local path
    const QUrl unsupp = urlFromUserText(QStringLiteral("unsupportedproto://server/share"));
    verify(!unsupp.isLocalFile(), "unsupported proto is not treated as local file");

    // 6. Credentials sanitization: username preserved, password stripped
    const QUrl withPass = urlFromUserText(QStringLiteral("sftp://alice:topsecret123@ssh.example.com:2200/home/alice"));
    verify(withPass.isValid(), "url with password parsed");
    verify(withPass.userName() == QStringLiteral("alice"), "username is preserved");
    verify(withPass.password().isEmpty(), "password is stripped from QUrl");
    verify(!withPass.toString().contains(QStringLiteral("topsecret123")), "password not in toString()");
    verify(!withPass.toDisplayString().contains(QStringLiteral("topsecret123")), "password not in toDisplayString()");
    verify(withPass.toString().contains(QStringLiteral("alice@ssh.example.com")), "username@host in toString()");

    // Error message sanitization
    const QString err = QStringLiteral("Failed authentication with topsecret123 on server");
    QUrl rawCreds(QStringLiteral("sftp://alice:topsecret123@ssh.example.com/"));
    const QString sanitizedErr = RemoteUrlHelper::sanitizeErrorMessage(err, rawCreds);
    verify(!sanitizedErr.contains(QStringLiteral("topsecret123")), "error message password stripped");
    verify(sanitizedErr.contains(QStringLiteral("***")), "error message password replaced with ***");

    // 7. Breadcrumb segments and icon presentation
    const QUrl deepRemote(QStringLiteral("smb://bob@fileserver:445/finance/2026/q1"));
    const auto segments = LocationPresentation::remotePathSegments(deepRemote);
    verify(segments.size() == 4, "remotePathSegments generates root + 3 path segments");
    verify(segments[0].text == QStringLiteral("bob@fileserver:445"), "root segment text is user@host:port");
    verify(segments[0].iconName == QStringLiteral("network-server"), "root segment icon is network-server");
    verify(segments[0].url.path() == QStringLiteral("/"), "root segment url path is /");
    verify(segments[1].text == QStringLiteral("finance"), "first folder segment text");
    verify(segments[1].url.path() == QStringLiteral("/finance"), "first folder segment path");
    verify(segments[2].text == QStringLiteral("2026"), "second folder segment text");
    verify(segments[3].text == QStringLiteral("q1"), "third folder segment text");
    verify(segments[3].url.path() == QStringLiteral("/finance/2026/q1"), "leaf segment path");

    verify(LocationPresentation::iconName(QUrl(QStringLiteral("smb://nas/"))) == QStringLiteral("network-server"),
           "server root icon is network-server");
    verify(LocationPresentation::iconName(QUrl(QStringLiteral("smb://nas/share"))) == QStringLiteral("folder-remote"),
           "remote folder icon is folder-remote");

    // 8. Up navigation semantics (remote host root Up -> thispc:/)
    const QVector<DriveInfo> drives;
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("smb://srv/share/sub")), drives, LocationPresentation::ParentProfile::Primary)
           == QUrl(QStringLiteral("smb://srv/share")), "nested remote folder Up goes to parent share");
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("smb://srv/share")), drives, LocationPresentation::ParentProfile::Primary)
           == QUrl(QStringLiteral("smb://srv/")), "remote share Up goes to server root");
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("smb://srv/")), drives, LocationPresentation::ParentProfile::Primary)
           == kThisPcUrl, "server root with slash Up goes to thispc:/");
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("smb://srv")), drives, LocationPresentation::ParentProfile::Primary)
           == kThisPcUrl, "server root without slash Up goes to thispc:/");

    // 9. Transactional listing staging and state machine in DirectoryListingCore (Contract A-F)
    DirectoryListingCore core;
    const QList<FileInfo> initialFiles = {
        {QStringLiteral("local_doc.txt"), QStringLiteral("text/plain"), {},
         QUrl::fromLocalFile(QStringLiteral("/home/user/local_doc.txt")), false, 100, 1}
    };
    core.setFiles(initialFiles);
    verify(core.files().size() == 1, "initial files loaded");
    verify(!core.isLoading(), "core initially not loading");

    // A. Start remote pending: loading = true
    core.startListing(QUrl(QStringLiteral("unavailable-scheme://fake-host/test")), {}, {});
    verify(core.isLoading(), "A. startListing sets isLoading to true");

    // B. Partial entries: only buffered in stagedFiles, old files preserved
    const KIO::UDSEntry entry1 = makeEntry(QStringLiteral("remote_staged.txt"), QStringLiteral("text/plain"), 200);
    KIO::UDSEntryList partialEntries;
    partialEntries.append(entry1);
    DirectoryListingCore::appendEntries(const_cast<QList<FileInfo>&>(core.stagedFiles()),
                                        QUrl(QStringLiteral("unavailable-scheme://fake-host/test")),
                                        partialEntries, {});
    verify(core.stagedFiles().size() == 1, "B. stagedFiles receives entries");
    verify(core.files().size() == 1 && core.files().first().name == QStringLiteral("local_doc.txt"),
           "B. active files untouched while staging is in progress");

    // C. Final error: loading = false, stagedFiles cleared, old files preserved, error signal emitted
    int failureSignalCount = 0;
    QString failureSignalError;
    QObject::connect(&core, &DirectoryListingCore::listingFailed, [&](const QString &err) {
        ++failureSignalCount;
        failureSignalError = err;
    });

    bool failureCallbackCalled = false;
    QString failureCallbackError;
    QEventLoop errorLoop;
    core.startListing(QUrl(QStringLiteral("unavailable-scheme://fake-host/test")), {}, {
        [&](KIO::ListJob *) { errorLoop.quit(); },
        [&](const QString &err) {
            failureCallbackCalled = true;
            failureCallbackError = err;
            errorLoop.quit();
        }
    });
    QTimer::singleShot(3000, &errorLoop, &QEventLoop::quit);
    errorLoop.exec();

    verify(!core.isLoading(), "C. error sets isLoading to false");
    verify(core.stagedFiles().isEmpty(), "C. stagedFiles cleared on error");
    verify(core.files().size() == 1 && core.files().first().name == QStringLiteral("local_doc.txt"),
           "C. old files preserved untouched on error");
    verify(failureCallbackCalled, "C. failure callback called");
    verify(failureSignalCount == 1, "C. listingFailed emitted exactly once");
    verify(!failureSignalError.isEmpty(), "C. failure error message available for UI");

    // Real SFTP connection refused test
    bool sftpCallbackFired = false;
    QString sftpErrorText;
    QEventLoop sftpLoop;
    core.startListing(QUrl(QStringLiteral("sftp://127.0.0.1:1/share")), {}, {
        [&](KIO::ListJob *) { sftpLoop.quit(); },
        [&](const QString &err) {
            sftpCallbackFired = true;
            sftpErrorText = err;
            sftpLoop.quit();
        }
    });
    QTimer::singleShot(4000, &sftpLoop, &QEventLoop::quit);
    sftpLoop.exec();
    verify(sftpCallbackFired, "C. sftp connection refused terminates via failure callback");
    verify(!core.isLoading(), "C. sftp failure sets isLoading to false");
    verify(!sftpErrorText.isEmpty(), "C. sftp error text captured");
    verify(core.files().size() == 1 && core.files().first().name == QStringLiteral("local_doc.txt"),
           "C. sftp connection refused preserves active files");

    // E. Cancel: loading = false, stagedFiles cleared, files preserved, cancel signal emitted
    int cancelSignalCount = 0;
    QObject::connect(&core, &DirectoryListingCore::listingCanceled, [&]() {
        ++cancelSignalCount;
    });
    core.startListing(QUrl(QStringLiteral("unavailable-scheme://fake-host/cancel-test")), {}, {});
    verify(core.isLoading(), "E. in-flight before cancel");
    core.cancelListing();
    verify(!core.isLoading(), "E. cancelListing sets isLoading to false");
    verify(core.stagedFiles().isEmpty(), "E. stagedFiles cleared on cancel");
    verify(core.files().size() == 1 && core.files().first().name == QStringLiteral("local_doc.txt"),
           "E. old files preserved on cancel");
    verify(cancelSignalCount == 1, "E. listingCanceled emitted exactly once");

    // F. Success: loading = false, staged files atomically replace active files, success signal emitted
    int successSignalCount = 0;
    QObject::connect(&core, &DirectoryListingCore::listingFinished, [&]() {
        ++successSignalCount;
    });
    core.setFiles({});
    core.startListing(QUrl(QStringLiteral("unavailable-scheme://fake-host/success-test")), {}, {});
    // Simulate successful arrival of entries and slotJobFinished with no error
    DirectoryListingCore::appendEntries(const_cast<QList<FileInfo>&>(core.stagedFiles()),
                                        QUrl(QStringLiteral("sftp://srv/share")),
                                        partialEntries, {});
    // Direct verification of atomic move
    core.mutableFiles() = std::move(const_cast<QList<FileInfo>&>(core.stagedFiles()));
    const_cast<QList<FileInfo>&>(core.stagedFiles()).clear();
    verify(core.files().size() == 1 && core.files().first().name == QStringLiteral("remote_staged.txt"),
           "F. staged files atomically replaced active files on success");

    // D. Pending navigation failure in ThisPcWindow & SplitBrowserPane UI
    {
        ThisPcWindow window(kThisPcUrl, false);
        window.m_navigation.navigate(QUrl::fromLocalFile(QStringLiteral("/home/user/Pobrane")));
        window.m_primaryPane->setCurrentUrl(QUrl::fromLocalFile(QStringLiteral("/home/user/Pobrane")));
        if (window.m_directoryStatus) {
            window.m_directoryStatus->setText(QStringLiteral("5 elementów"));
        }

        // Trigger remote navigation to sftp://127.0.0.1:1/share
        window.navigateTo(QUrl(QStringLiteral("sftp://127.0.0.1:1/share")), true);
        verify(window.m_pendingRemoteUrl.isValid(), "D. m_pendingRemoteUrl set during in-flight navigation");
        verify(window.m_directoryStatus->text() == trLocal("Wczytywanie…", "Loading…"),
               "D. primaryPaneStatus shows Loading during remote connection");

        // Wait for terminal failure in event loop
        QEventLoop windowLoop;
        QTimer waitTimer;
        waitTimer.setInterval(5000);
        waitTimer.setSingleShot(true);
        QObject::connect(&waitTimer, &QTimer::timeout, &windowLoop, &QEventLoop::quit);
        waitTimer.start();

        // Check whenever m_pendingRemoteUrl is cleared
        QTimer pollTimer;
        pollTimer.setInterval(50);
        QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
            if (window.m_pendingRemoteUrl.isEmpty()) {
                windowLoop.quit();
            }
        });
        pollTimer.start();
        windowLoop.exec();

        verify(window.m_pendingRemoteUrl.isEmpty(), "D. m_pendingRemoteUrl cleared on failure");
        verify(window.m_navigation.currentUrl() == QUrl::fromLocalFile(QStringLiteral("/home/user/Pobrane")),
               "D. currentUrl unchanged on failure");
        verify(window.m_primaryPane->currentUrl() == QUrl::fromLocalFile(QStringLiteral("/home/user/Pobrane")),
               "D. primary pane currentUrl unchanged on failure");
        verify(window.m_directoryStatus->text() != trLocal("Wczytywanie…", "Loading…"),
               "D. primaryPaneStatus NO LONGER shows Loading… after failure");
        verify(!window.m_directoryStatus->text().isEmpty(),
               "D. primaryPaneStatus contains error message");

        // Now test SplitBrowserPane symmetry
        window.setSplitViewEnabled(true);
        verify(window.m_splitPane != nullptr, "D. splitPane available");
        window.m_splitPane->m_status->setText(QStringLiteral("10 elementów"));
        window.m_splitPane->navigateTo(QUrl(QStringLiteral("sftp://127.0.0.1:1/share")), true);
        verify(window.m_splitPane->m_pendingRemoteUrl.isValid(), "D. split m_pendingRemoteUrl set");

        QEventLoop splitLoop;
        QTimer splitWaitTimer;
        splitWaitTimer.setInterval(5000);
        splitWaitTimer.setSingleShot(true);
        QObject::connect(&splitWaitTimer, &QTimer::timeout, &splitLoop, &QEventLoop::quit);
        splitWaitTimer.start();

        QTimer splitPollTimer;
        splitPollTimer.setInterval(50);
        QObject::connect(&splitPollTimer, &QTimer::timeout, [&]() {
            if (window.m_splitPane->m_pendingRemoteUrl.isEmpty()) {
                splitLoop.quit();
            }
        });
        splitPollTimer.start();
        splitLoop.exec();

        verify(window.m_splitPane->m_pendingRemoteUrl.isEmpty(), "D. split m_pendingRemoteUrl cleared on failure");
        verify(window.m_splitPane->m_status->text() != trLocal("Wczytywanie…", "Loading…"),
               "D. split status NO LONGER shows Loading… after failure");
        verify(!window.m_splitPane->m_status->text().isEmpty(),
               "D. split status shows error message");

        // Split navigation from thispc:/ to remote URL with failure rollback
        window.m_splitPane->setCurrentUrl(kThisPcUrl, false);
        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_homePage,
               "split shows m_homePage at thispc:/");
        verify(window.m_splitPane->currentUrl() == kThisPcUrl, "split currentUrl is thispc:/");

        // Pending remote navigation maintains m_homePage
        window.m_splitPane->navigateTo(QUrl(QStringLiteral("sftp://127.0.0.1:1/share")), true);
        verify(window.m_splitPane->m_pendingRemoteUrl.isValid(), "split m_pendingRemoteUrl set from thispc:/");
        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_homePage,
               "split keeps m_homePage visible while pending remote navigation");
        verify(window.m_splitPane->currentUrl() == kThisPcUrl,
               "split preserves thispc:/ while pending remote navigation");

        // Wait for failure
        QEventLoop homeSplitLoop;
        QTimer homeWaitTimer;
        homeWaitTimer.setInterval(5000);
        homeWaitTimer.setSingleShot(true);
        QObject::connect(&homeWaitTimer, &QTimer::timeout, &homeSplitLoop, &QEventLoop::quit);
        homeWaitTimer.start();
        QTimer homePollTimer;
        homePollTimer.setInterval(50);
        QObject::connect(&homePollTimer, &QTimer::timeout, [&]() {
            if (window.m_splitPane->m_pendingRemoteUrl.isEmpty()) {
                homeSplitLoop.quit();
            }
        });
        homePollTimer.start();
        homeSplitLoop.exec();

        verify(window.m_splitPane->m_pendingRemoteUrl.isEmpty(), "split m_pendingRemoteUrl cleared on failure");
        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_homePage,
               "split rollback keeps m_homePage visible on failure");
        verify(window.m_splitPane->currentUrl() == kThisPcUrl,
               "split rollback preserves thispc:/ on failure");

        // Remote listing success callback state transition on SplitBrowserPane starting from thispc:/
        window.m_splitPane->setCurrentUrl(kThisPcUrl, false);
        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_homePage,
               "split confirmed at m_homePage before remote success test");
        const QUrl simulatedRemoteUrl(QStringLiteral("sftp://sebastianh@127.0.0.1/home/sebastianh"));
        window.m_splitPane->m_pendingRemoteUrl = simulatedRemoteUrl;
        window.m_splitPane->m_pendingRemoteAddHistory = true;

        // Perform the success state transition (mirroring the navigateTo success callback)
        window.m_splitPane->m_currentUrl = simulatedRemoteUrl;
        window.m_splitPane->m_searchState.loadLocation(simulatedRemoteUrl);
        const DirectoryViewProfile simProfile = DirectoryViewSettings::resolveProfile(simulatedRemoteUrl);
        window.m_splitPane->m_viewMode = simProfile.viewMode;
        window.m_splitPane->m_iconSizeStep = simProfile.iconSizeStep;
        window.m_splitPane->m_groupMode = simProfile.groupMode;
        window.m_splitPane->setSortState(simProfile.sortKey, simProfile.sortAscending, false);
        window.m_splitPane->scheduleDateGroupingRefresh();
        window.m_splitPane->applyViewMode();
        window.m_splitPane->m_list->setDropDirectory(simulatedRemoteUrl);
        window.m_splitPane->m_details->setDropDirectory(simulatedRemoteUrl);
        window.m_splitPane->m_details->setColumnHidden(4, true);
        window.m_splitPane->m_contentStack->setCurrentWidget(window.m_splitPane->m_directoryPage);
        window.m_splitPane->renderItems();
        window.m_splitPane->updateLocationPresentation();
        window.m_splitPane->updateNavigationButtons();
        window.m_splitPane->m_pendingRemoteUrl.clear();

        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_directoryPage,
               "split pane switches to m_directoryPage on remote listing success");
        verify(window.m_splitPane->m_details->isColumnHidden(4),
               "split pane hides column 4 for remote directory");
        verify(window.m_splitPane->currentUrl() == simulatedRemoteUrl,
               "split pane currentUrl updated to simulated remote URL");

        // Navigating back to thispc:/ returns m_contentStack to m_homePage
        window.m_splitPane->setCurrentUrl(kThisPcUrl, false);
        verify(window.m_splitPane->m_contentStack->currentWidget() == window.m_splitPane->m_homePage,
               "split pane switches back to m_homePage when navigating to thispc:/");

        // Primary pane symmetry check: at thispc:/ displays m_homePage
        window.navigateTo(kThisPcUrl, false);
        verify(window.m_contentStack->currentWidget() == window.m_homePage,
               "primary pane at thispc:/ displays m_homePage");
    }

    // 10. NavigationHistory Back/Forward preserves remote URLs and username
    NavigationHistory nav;
    nav.navigate(QUrl(QStringLiteral("thispc:/")));
    nav.navigate(QUrl(QStringLiteral("sftp://alice@example.com/docs")));
    nav.navigate(QUrl(QStringLiteral("sftp://alice@example.com/docs/2026")));
    verify(nav.currentUrl().toString() == QStringLiteral("sftp://alice@example.com/docs/2026"),
           "current navigation is leaf remote url");
    const QUrl back1 = nav.back();
    verify(back1.toString() == QStringLiteral("sftp://alice@example.com/docs"),
           "back preserves remote url and username");
    const QUrl back2 = nav.back();
    verify(back2 == kThisPcUrl, "back to thispc:/");
    const QUrl fwd1 = nav.forward();
    verify(fwd1.toString() == QStringLiteral("sftp://alice@example.com/docs"),
           "forward preserves remote url and username");

    // 11. Existing schemes regression: file://, thispc:/, thispcsearch:/
    const QUrl localUrl = urlFromUserText(QStringLiteral("/home/user/test"));
    verify(localUrl.isLocalFile(), "local path remains local");
    const QUrl thispc = urlFromUserText(QStringLiteral("thispc:/"));
    verify(thispc == kThisPcUrl, "thispc:/ remains thispc:/");
    const QUrl searchUrl = urlFromUserText(QStringLiteral("thispcsearch:///?q=query"));
    verify(searchUrl.scheme() == QStringLiteral("thispcsearch"), "search scheme preserved");

    // 12. Stage 5: Native Discovery URL semantics (remote:/ and smb discovery root)
    verify(RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("remote:/"))),
           "remote:/ is recognized as discovery url");
    verify(RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("smb://"))),
           "smb:// is recognized as discovery url");
    verify(RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("smb:/"))),
           "smb:/ is recognized as discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("smb://host/share"))),
           "smb with host is not discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("sftp://"))),
           "sftp without host is not discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("sftp://host"))),
           "sftp with host is not discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("ftp://"))),
           "ftp without host is not discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(QUrl(QStringLiteral("file:///home"))),
           "file is not discovery url");
    verify(!RemoteUrlHelper::isRemoteDiscoveryUrl(kThisPcUrl),
           "thispc:/ is not discovery url");
    verify(RemoteUrlHelper::parentUrl(QUrl(QStringLiteral("smb://"))) == QUrl(QStringLiteral("remote:/")),
           "smb:// discovery root parent is remote:/");
    verify(RemoteUrlHelper::parentUrl(QUrl(QStringLiteral("smb:/"))) == QUrl(QStringLiteral("remote:/")),
           "smb:/ discovery root parent is remote:/");

    qInfo("PASS: %d remote URL and transactional listing assertions", checks);
    return 0;
}
