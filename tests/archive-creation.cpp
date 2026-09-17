// Real archive creation tests. All sources and output are disposable.
static int checks = 0;
static void verify(bool condition, const char *message)
{
    if (!condition) qFatal("FAIL: %s", message);
    ++checks;
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create disposable input");
    verify(file.write(data) == data.size(), "write disposable input");
}

static QHash<QString, QByteArray> archiveContents(const QString &path)
{
    struct archive *reader = archive_read_new();
    verify(reader != nullptr, "create archive reader");
    verify(archive_read_support_filter_all(reader) == ARCHIVE_OK, "archive filters");
    verify(archive_read_support_format_all(reader) == ARCHIVE_OK, "archive formats");
    verify(archive_read_open_filename(reader, QFile::encodeName(path).constData(), 65536) == ARCHIVE_OK,
           "open created archive");
    QHash<QString, QByteArray> contents;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        verify(!contents.contains(name), "no duplicate archive members");
        QByteArray data;
        char buffer[65536];
        la_ssize_t length = 0;
        while ((length = archive_read_data(reader, buffer, sizeof(buffer))) > 0)
            data.append(buffer, static_cast<qsizetype>(length));
        verify(length == 0, "read entire archive member");
        contents.insert(name, data);
    }
    verify(status == ARCHIVE_EOF, "complete archive parse");
    verify(archive_read_close(reader) == ARCHIVE_OK, "close archive reader");
    verify(archive_read_free(reader) == ARCHIVE_OK, "release archive reader");
    return contents;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // libarchive's reader also requires a UTF-8 C locale for 7z filenames.
    verify(::setlocale(LC_CTYPE, "C.UTF-8") != nullptr, "UTF-8 test locale available");
    QTemporaryDir root;
    verify(root.isValid(), "disposable test root");
    const QString input = root.filePath(QStringLiteral("wejście żółw ; $()"));
    const QString output = root.filePath(QStringLiteral("wynik ą"));
    verify(QDir().mkpath(input + QStringLiteral("/katalog/empty")), "create input tree");
    verify(QDir().mkpath(output), "create output directory");
    const QByteArray payload("UTF-8 filename and literal shell characters\n");
    const QString file = input + QStringLiteral("/żółw ; $(no-shell).txt");
    const QString nested = input + QStringLiteral("/katalog/child.txt");
    writeFile(file, payload);
    writeFile(nested, "nested data");
    const QList<QUrl> sources {QUrl::fromLocalFile(file), QUrl::fromLocalFile(input + "/katalog")};
    int index = 0;
    auto run = [&](const QList<QUrl> &selected, const QString &target, ThisPcArchiveFormat format, bool expected) {
        auto *job = new ArchiveCreationJob(selected, QUrl::fromLocalFile(target), format);
        job->setAutoDelete(false);
        int results = 0;
        QObject::connect(job, &KJob::result, &app, [&](KJob *) { ++results; });
        job->start();
        verify(QTest::qWaitFor([&] { return results == 1; }, 30000), "job finishes asynchronously");
        if ((job->error() == 0) != expected) qWarning() << "Unexpected archive job result:" << job->errorText();
        verify((job->error() == 0) == expected, "expected job outcome");
        QTest::qWait(10);
        verify(results == 1, "exactly one result signal");
        verify(job->publishedUrl().isEmpty() != expected, "published URL only on success");
        delete job;
        verify(QDir(output).entryList({".thispc-create-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
               "private staging always removed");
    };
    for (const auto format : {ThisPcArchiveFormat::Zip, ThisPcArchiveFormat::SevenZip, ThisPcArchiveFormat::TarGzip}) {
        const QString target = output + "/created-" + QString::number(++index) + thispcArchiveSuffix(format);
        run(sources, target, format, true);
        const auto contents = archiveContents(target);
        verify(contents.value(QStringLiteral("żółw ; $(no-shell).txt")) == payload, "source bytes and Unicode name preserved");
        verify(contents.value("katalog/child.txt") == "nested data", "nested file roundtrip");
        verify(contents.contains("katalog/empty/"), "empty directory retained");
        verify(!QFileInfo::exists(input + "/no-shell"), "no shell interpretation");
        const QByteArray original = [&] { QFile f(target); verify(f.open(QIODevice::ReadOnly), "read output sentinel"); return f.readAll(); }();
        run(sources, target, format, false);
        QFile unchanged(target);
        verify(unchanged.open(QIODevice::ReadOnly) && unchanged.readAll() == original,
               "collision never modifies existing archive");
    }
    const QString bad = input + "/symbolic-link";
    verify(QFile::link(file, bad), "make symlink fixture");
    run({QUrl::fromLocalFile(bad)}, output + "/reject.zip", ThisPcArchiveFormat::Zip, false);
    verify(!QFileInfo::exists(output + "/reject.zip"), "symlink source rejected");
    run({QUrl::fromLocalFile(input + "/katalog")}, input + "/katalog/inside.zip", ThisPcArchiveFormat::Zip, false);
    verify(!QFileInfo::exists(input + "/katalog/inside.zip"), "archive inside source directory rejected");
    const QString dangling = output + "/dangling.zip";
    verify(QFile::link(root.filePath("nonexistent"), dangling), "create dangling target symlink");
    run(sources, dangling, ThisPcArchiveFormat::Zip, false);
    verify(QFileInfo(dangling).isSymLink(), "existing dangling symlink is never overwritten");
    run({QUrl::fromLocalFile(input + "/absent")}, output + "/missing.zip", ThisPcArchiveFormat::Zip, false);

    // Kill after a progress update while substantial incompressible content remains.
    QByteArray randomData(64 * 1024 * 1024, '\0');
    quint32 state = 0x12345678;
    for (qsizetype i = 0; i < randomData.size(); ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        randomData[i] = static_cast<char>(state);
    }
    const QString large = input + "/large.dat";
    writeFile(large, randomData);
    randomData.clear();
    const QString cancelled = output + "/cancelled.zip";
    auto *job = new ArchiveCreationJob({QUrl::fromLocalFile(large)}, QUrl::fromLocalFile(cancelled), ThisPcArchiveFormat::Zip);
    job->setAutoDelete(false);
    int resultSignals = 0;
    QObject::connect(job, &KJob::result, &app, [&](KJob *) { ++resultSignals; });
    QObject::connect(job, &KJob::processedAmountChanged, &app, [&](KJob *, KJob::Unit unit, qulonglong bytes) {
        if (unit == KJob::Bytes && bytes >= 4 * 1024 * 1024 && !resultSignals)
            verify(job->kill(KJob::EmitResult), "creation cancellation accepted");
    });
    job->start();
    verify(QTest::qWaitFor([&] { return resultSignals == 1; }, 30000), "cancelled job signals completion");
    verify(job->error() != 0, "cancelled job does not report success");
    delete job;
    verify(!QFileInfo::exists(cancelled), "cancelled archive not published");
    verify(QDir(output).entryList({".thispc-create-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
           "cancelled staging removed");
    qInfo() << "archive_creation PASS:" << checks << "assertions";
    return 0;
}
