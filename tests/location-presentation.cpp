// Appended to the temporary regression binary by run-pane-actions.py.

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) qFatal("FAIL: %s", message);
    };

    const QUrl thisPc(QStringLiteral("thispc:/"));
    const QUrl root = QUrl::fromLocalFile(QStringLiteral("/"));
    const QUrl home = QUrl::fromLocalFile(QDir::homePath());
    const QUrl local = QUrl::fromLocalFile(QStringLiteral("/etc/hosts"));
    const QUrl driveRoot = QUrl::fromLocalFile(QStringLiteral("/mnt/stage2-drive"));
    const QUrl driveChild = QUrl::fromLocalFile(QStringLiteral("/mnt/stage2-drive/folder/file.txt"));
    const QUrl admin(QStringLiteral("admin:/etc/hosts"));
    const QUrl search = makeSearchLocation(
        QStringLiteral("needle"), 0, QUrl::fromLocalFile(QStringLiteral("/tmp/base")), 0, 0, 0);
    const QUrl searchWithoutBase = makeSearchLocation(
        QStringLiteral("needle"), 2, {}, 0, 0, 0);
    const QUrl remote(QStringLiteral("sftp://example.test/share/folder"));
    const QVector<DriveInfo> drives{{QStringLiteral("Stage 2 Drive"), {}, {}, {}, {}, {},
                                     driveRoot, QStringLiteral("drive-harddisk"), 0}};

    verify(LocationPresentation::primaryTitle(thisPc, drives) == trLocal("Ten komputer", "This PC"),
           "This PC primary title");
    verify(LocationPresentation::splitTitle(thisPc) == LocationPresentation::primaryTitle(thisPc, drives),
           "This PC title consistency");
    verify(LocationPresentation::iconName(thisPc) == QStringLiteral("computer"),
           "This PC icon");
    verify(!LocationPresentation::parentUrl(thisPc, drives, LocationPresentation::ParentProfile::Primary).isValid(),
           "This PC has no primary parent");
    verify(!LocationPresentation::parentUrl(thisPc, drives, LocationPresentation::ParentProfile::Split).isValid(),
           "This PC has no split parent");

    verify(LocationPresentation::primaryTitle(root, drives) == QStringLiteral("/"), "local root title");
    verify(LocationPresentation::parentUrl(root, drives, LocationPresentation::ParentProfile::Primary) == thisPc,
           "local root parent");
    verify(LocationPresentation::iconName(root) == QStringLiteral("drive-harddisk"), "local root icon");

    verify(LocationPresentation::primaryTitle(home, drives) == trLocal("Katalog domowy", "Home"),
           "home primary title");
    const auto homeSegments = LocationPresentation::localPathSegments(home, drives);
    verify(homeSegments.size() == 1 && homeSegments.first().url == home,
           "home is one breadcrumb segment");

    verify(LocationPresentation::primaryTitle(local, drives) == QStringLiteral("hosts"),
           "local leaf primary title");
    verify(LocationPresentation::splitTitle(local) == QStringLiteral("hosts"),
           "local leaf split title");
    verify(LocationPresentation::primaryTitle(local, drives) == LocationPresentation::splitTitle(local),
           "local leaf title consistency");
    verify(LocationPresentation::parentUrl(local, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl::fromLocalFile(QStringLiteral("/etc")),
           "local file parent");
    const auto localSegments = LocationPresentation::localPathSegments(local, drives);
    verify(localSegments.size() == 3, "local breadcrumb segment count");
    verify(localSegments.at(0).text == QStringLiteral("/")
               && localSegments.at(1).text == QStringLiteral("etc")
               && localSegments.at(2).text == QStringLiteral("hosts"),
           "local breadcrumb labels");
    verify(localSegments.at(2).url == local, "local breadcrumb cumulative URL");
    verify(LocationPresentation::contentHeaderText(local) == urlForDisplay(local),
           "local content header uses the full canonical path");

    verify(LocationPresentation::primaryTitle(driveRoot, drives) == QStringLiteral("Stage 2 Drive"),
           "simulated drive title");
    verify(LocationPresentation::parentUrl(driveRoot, drives, LocationPresentation::ParentProfile::Primary) == thisPc,
           "simulated drive parent");
    const auto driveSegments = LocationPresentation::localPathSegments(driveChild, drives);
    verify(driveSegments.size() == 4, "drive breadcrumb segment count");
    verify(driveSegments.at(0).url == thisPc && driveSegments.at(1).url == driveRoot,
           "drive breadcrumbs start at This PC and drive root");
    verify(driveSegments.last().url == driveChild, "drive breadcrumb cumulative URL");

    verify(LocationPresentation::splitLocationText(admin).contains(QStringLiteral("etc")),
           "admin friendly location text");
    verify(LocationPresentation::iconName(admin) == QStringLiteral("security-high"), "admin icon");
    const auto adminSegments = LocationPresentation::adminPathSegments(admin);
    verify(adminSegments.size() == 3 && adminSegments.first().url == QUrl(QStringLiteral("admin:/"))
               && adminSegments.last().url == admin,
           "admin breadcrumb data");
    verify(LocationPresentation::contentHeaderText(admin) == urlForDisplay(admin),
           "admin content header preserves remote URL semantics");

    verify(LocationPresentation::primaryTitle(search, drives) == LocationPresentation::splitTitle(search),
           "search title consistency");
    verify(LocationPresentation::iconName(search) == QStringLiteral("system-search"), "search icon");
    verify(LocationPresentation::parentUrl(search, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl::fromLocalFile(QStringLiteral("/tmp/base")),
           "search parent is encoded base");
    verify(LocationPresentation::parentUrl(searchWithoutBase, drives, LocationPresentation::ParentProfile::Split)
               == thisPc,
           "search without base parent is This PC");
    verify(LocationPresentation::contentHeaderText(search)
               == LocationPresentation::primaryTitle(search, drives),
           "search content header keeps its dedicated presentation");

    verify(LocationPresentation::primaryTitle(remote, drives) == QStringLiteral("folder"),
           "remote primary title");
    verify(LocationPresentation::splitTitle(remote) == LocationPresentation::primaryTitle(remote, drives),
           "remote title consistency");
    verify(LocationPresentation::splitLocationText(remote) == urlForDisplay(remote),
           "remote friendly text");
    verify(LocationPresentation::contentHeaderText(remote) == urlForDisplay(remote),
           "remote content header never assumes a local filesystem path");
    verify(LocationPresentation::parentUrl(remote, drives, LocationPresentation::ParentProfile::Primary)
               == QUrl(QStringLiteral("sftp://example.test/share")),
           "remote parent");
    verify(LocationPresentation::parentUrl(QUrl(QStringLiteral("sftp://example.test/")), drives,
                                           LocationPresentation::ParentProfile::Split) == thisPc,
           "remote root parent edge case");

    const QUrl queried(QStringLiteral("sftp://example.test/share/folder?keep=1"));
    verify(LocationPresentation::parentUrl(queried, drives, LocationPresentation::ParentProfile::Primary).hasQuery(),
           "primary parent preserves query");
    verify(!LocationPresentation::parentUrl(queried, drives, LocationPresentation::ParentProfile::Split).hasQuery(),
           "split parent clears query");
    verify(!LocationPresentation::parentUrl(QUrl(), drives, LocationPresentation::ParentProfile::Split).isValid(),
           "invalid split location has no parent");

    qInfo("PASS: %d location presentation assertions", checks);
    return 0;
}
