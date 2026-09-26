#include "directorylistingcore.h"
#include "directoryviewsettings.h"

#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QImage>
#include <QMimeDatabase>
#include <QTemporaryDir>
#include <QTimer>

#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif

static int checks = 0;

static void verify(bool condition, const char *description)
{
    if (!condition) qFatal("FAIL: %s", description);
    ++checks;
}

static KIO::UDSEntry entry(const QString &name, const QString &displayName,
    const QString &mime, qint64 size, qint64 modified, const QString &icon = {})
{
    KIO::UDSEntry value;
    value.fastInsert(KIO::UDSEntry::UDS_NAME, name);
    if (!displayName.isNull())
        value.fastInsert(KIO::UDSEntry::UDS_DISPLAY_NAME, displayName);
    value.fastInsert(KIO::UDSEntry::UDS_MIME_TYPE, mime);
    value.fastInsert(KIO::UDSEntry::UDS_SIZE, size);
    value.fastInsert(KIO::UDSEntry::UDS_MODIFICATION_TIME, modified);
    if (!icon.isEmpty()) value.fastInsert(KIO::UDSEntry::UDS_ICON_NAME, icon);
#ifndef Q_OS_WIN
    value.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE,
        mime == QStringLiteral("inode/directory") ? S_IFDIR : S_IFREG);
#endif
    return value;
}

static QList<FileInfo> sampleFiles(const QUrl &base)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return {
        {QStringLiteral("folder"), QStringLiteral("inode/directory"), {},
         childUrlWithName(base, QStringLiteral("folder")), true, -1, now},
        {QStringLiteral("z.png"), QStringLiteral("image/png"), {},
         childUrlWithName(base, QStringLiteral("z.png")), false, 4096, now - 86400 * 40},
        {QStringLiteral("a.txt"), QStringLiteral("text/plain"), {},
         childUrlWithName(base, QStringLiteral("a.txt")), false, 0, now - 86400 * 2},
        {QStringLiteral("m.bin"), QStringLiteral("application/octet-stream"), {},
         childUrlWithName(base, QStringLiteral("m.bin")), false, 12, now - 60},
    };
}

static QStringList names(const DirectoryListingCore::PreparedListing &listing)
{
    QStringList result;
    for (const auto &file : listing.files) result.push_back(file.file.name);
    return result;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QMimeDatabase mimeDatabase;
    const QUrl base(QStringLiteral("sftp://example.test/home/user"));
    const qint64 modified = 123456789;
    const auto mappedEntry = entry(QStringLiteral("raw.txt"), QStringLiteral("Shown.txt"),
                                   QStringLiteral("text/plain"), 42, modified,
                                   QStringLiteral("text-x-script"));
    const FileInfo mapped = DirectoryListingCore::mapEntry(base, mappedEntry);
    verify(mapped.name == QStringLiteral("Shown.txt"), "UDS display name maps to FileInfo name");
    verify(mapped.mimeType == QStringLiteral("text/plain") && mapped.iconName == QStringLiteral("text-x-script"),
           "UDS mime and icon map to FileInfo");
    verify(mapped.url == QUrl(QStringLiteral("sftp://example.test/home/user/raw.txt")),
           "UDS child URL preserves remote KIO base");
    verify(!mapped.isDir && mapped.size == 42 && mapped.modificationTime == modified,
           "UDS type size and modification time map to FileInfo");

    KIO::UDSEntryList entries = {
        entry(QStringLiteral("."), {}, QStringLiteral("inode/directory"), -1, 0),
        entry(QStringLiteral(".."), {}, QStringLiteral("inode/directory"), -1, 0),
        entry(QStringLiteral(".hidden"), {}, QStringLiteral("text/plain"), 1, 0),
        entry(QStringLiteral("shown"), {}, QStringLiteral("text/plain"), 2, 0),
    };
    QList<FileInfo> filtered;
    DirectoryListingCore::ListingOptions listingOptions;
    DirectoryListingCore::appendEntries(filtered, base, entries, listingOptions);
    verify(filtered.size() == 1 && filtered.first().name == QStringLiteral("shown"),
           "dot entries and hidden entries are filtered by default");
    filtered.clear();
    listingOptions.showHiddenFiles = true;
    DirectoryListingCore::appendEntries(filtered, base, entries, listingOptions);
    verify(filtered.size() == 2 && filtered.first().name == QStringLiteral(".hidden"),
           "hidden entries are retained when enabled while dot entries stay filtered");
    const auto displayOnly = entry(QString(), QStringLiteral("display-only"),
                                   QStringLiteral("text/plain"), 0, 0);
    const FileInfo displayOnlyFile = DirectoryListingCore::mapEntry(base, displayOnly);
    listingOptions.emptyNamePolicy = DirectoryListingCore::EmptyNamePolicy::DisplayName;
    verify(DirectoryListingCore::acceptsEntry(displayOnly, displayOnlyFile, listingOptions),
           "Primary display-name policy preserves historical display-only entry behavior");
    listingOptions.emptyNamePolicy = DirectoryListingCore::EmptyNamePolicy::RawName;
    verify(!DirectoryListingCore::acceptsEntry(displayOnly, displayOnlyFile, listingOptions),
           "Split raw-name policy preserves historical empty raw-name filtering");

    DirectoryListingCore core;
    DirectoryListingCore::RenderOptions renderOptions;
    const auto samples = sampleFiles(base);
    for (int key = 0; key < 4; ++key) {
        core.setFiles(samples);
        renderOptions.sortKey = key;
        renderOptions.sortAscending = true;
        auto ascending = core.prepare(renderOptions, mimeDatabase);
        verify(ascending.files.size() == samples.size() && ascending.files.first().file.isDir,
               "ascending sort retains every item and keeps folders first");
        core.setFiles(samples);
        renderOptions.sortAscending = false;
        auto descending = core.prepare(renderOptions, mimeDatabase);
        verify(descending.files.size() == samples.size() && descending.files.first().file.isDir,
               "descending sort retains every item and keeps folders first");
        QStringList ascFiles = names(ascending); ascFiles.removeFirst();
        QStringList descFiles = names(descending); descFiles.removeFirst();
        std::reverse(ascFiles.begin(), ascFiles.end());
        verify(ascFiles == descFiles, "name type size and date sorts reverse deterministically");
    }

    core.setFiles(samples);
    renderOptions = {};
    renderOptions.acceptsFile = [](const FileInfo &file, QMimeDatabase &) {
        return file.name.endsWith(QStringLiteral(".txt"));
    };
    auto filteredRender = core.prepare(renderOptions, mimeDatabase);
    verify(filteredRender.visibleCount == 1 && filteredRender.totalCount == 4
               && filteredRender.files.first().file.name == QStringLiteral("a.txt"),
           "neutral render filter reports visible and total counts");

    for (int grouping : {DirectoryViewSettings::NoGrouping,
                         DirectoryViewSettings::GroupByType,
                         DirectoryViewSettings::GroupByDate,
                         DirectoryViewSettings::GroupBySize}) {
        core.setFiles(samples);
        renderOptions = {};
        renderOptions.groupMode = grouping;
        const auto grouped = core.prepare(renderOptions, mimeDatabase);
        verify(grouped.files.size() == samples.size(), "all grouping modes preserve item count");
        if (grouping != DirectoryViewSettings::NoGrouping) {
            bool ordered = true;
            for (int row = 1; row < grouped.files.size(); ++row)
                ordered &= grouped.files.at(row - 1).categorySort.localeAwareCompare(
                    grouped.files.at(row).categorySort) <= 0;
            verify(ordered, "Type Date and Size groups have deterministic category order");
        }
    }

    core.setFiles(samples);
    renderOptions = {};
    const auto selectionListing = core.prepare(renderOptions, mimeDatabase);
    const QSet<QString> selected = {
        samples.at(1).url.toString(), samples.at(3).url.toString(),
        QStringLiteral("file:///missing")};
    const QList<int> selectedRows = DirectoryListingCore::rowsForSelection(
        selectionListing.files, selected);
    verify(selectedRows.size() == 2
               && selectionListing.files.at(selectedRows.at(0)).file.url.toString() != QStringLiteral("file:///missing"),
           "selection mapping restores existing URLs and ignores missing URLs");

    FileInfo folder = samples.first();
    FileInfo unknown{QStringLiteral("unknown"), {}, {}, QUrl(QStringLiteral("sftp://host/unknown")), false, 1, 0};
    FileInfo text{QStringLiteral("note.txt"), QStringLiteral("text/plain"), QStringLiteral("text-x-generic"),
                  QUrl(QStringLiteral("sftp://host/note.txt")), false, 1, 0};
    verify(!DirectoryListingCore::resolvedIconName(folder, mimeDatabase).isEmpty(),
           "folder resolves to a directory icon with folder fallback available");
    verify(!DirectoryListingCore::resolvedIconName(text, mimeDatabase).isEmpty(),
           "generic icon resolves through known MIME type");
    verify(DirectoryListingCore::resolvedIconName(unknown, mimeDatabase) == QStringLiteral("unknown"),
           "unknown file icon fallback is explicit");

    QTemporaryDir temporary;
    verify(temporary.isValid(), "thumbnail test temporary directory exists");
    const QString imagePath = temporary.filePath(QStringLiteral("sample.png"));
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    verify(image.save(imagePath), "thumbnail test image is written");
    FileInfo imageFile{QStringLiteral("sample.png"), QStringLiteral("image/png"), {},
                       QUrl::fromLocalFile(imagePath), false, QFileInfo(imagePath).size(), 1};
    core.clearThumbnailCache();
    verify(!core.iconForFile(imageFile, mimeDatabase, true, true).isNull()
               && core.thumbnailCacheSize() == 1,
           "Primary thumbnail path decodes and caches a local image");
    core.iconForFile(imageFile, mimeDatabase, true, true);
    verify(core.thumbnailCacheSize() == 1, "cached thumbnail is reused by stable identity");
    core.clearThumbnailCache();
    verify(!core.iconForFile(imageFile, mimeDatabase, true, false).isNull()
               && core.thumbnailCacheSize() == 0,
           "Split thumbnail adapter decodes without populating the cache");

    DirectoryListingCore primaryAdapter;
    DirectoryListingCore splitAdapter;
    primaryAdapter.setFiles(samples);
    splitAdapter.setFiles(samples);
    renderOptions = {};
    renderOptions.groupMode = DirectoryViewSettings::GroupByType;
    const auto primaryOutput = primaryAdapter.prepare(renderOptions, mimeDatabase);
    const auto splitOutput = splitAdapter.prepare(renderOptions, mimeDatabase);
    verify(names(primaryOutput) == names(splitOutput),
           "Primary and Split adapters produce equivalent ordered items for equivalent data");
    bool categoriesEqual = primaryOutput.files.size() == splitOutput.files.size();
    for (int row = 0; categoriesEqual && row < primaryOutput.files.size(); ++row)
        categoriesEqual = primaryOutput.files.at(row).categorySort == splitOutput.files.at(row).categorySort;
    verify(categoriesEqual, "Primary and Split adapters produce equivalent group metadata");

    bool canceledCallback = false;
    core.startListing(QUrl::fromLocalFile(temporary.path()), {}, {
        [&](KIO::ListJob *) { canceledCallback = true; },
        [&](const QString &) { canceledCallback = true; }});
    verify(core.listingJob() != nullptr, "listing lifecycle exposes the active KIO job");
    core.cancelListing();
    verify(core.listingJob() == nullptr, "cancel resets active job state synchronously");
    QEventLoop canceledLoop;
    QTimer::singleShot(100, &canceledLoop, &QEventLoop::quit);
    canceledLoop.exec();
    verify(!canceledCallback, "canceled listing ignores stale completion callbacks");

    bool failed = false;
    QEventLoop errorLoop;
    core.startListing(QUrl(QStringLiteral("unavailable-thispc-core:/missing")), {}, {
        [&](KIO::ListJob *) { errorLoop.quit(); },
        [&](const QString &) { failed = true; errorLoop.quit(); }});
    QTimer::singleShot(3000, &errorLoop, &QEventLoop::quit);
    errorLoop.exec();
    verify(failed && core.listingJob() == nullptr,
           "error completion invokes failure adapter and resets job state");

    qInfo("PASS: %d DirectoryListingCore assertions", checks);
    return 0;
}
