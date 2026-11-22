/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

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

    // =========================================================================
    // STAGE 1B.7B (SEC-046-04): ARCHIVE POSIX PERMISSION TESTS A1–A8
    // =========================================================================
    auto statModeAC = [](const QString &path) -> mode_t {
        struct stat st {};
        verify(::lstat(QFile::encodeName(path).constData(), &st) == 0, "lstat archive test path");
        return st.st_mode & 07777;
    };
    auto archiveEntryModes = [](const QString &path) -> QHash<QString, mode_t> {
        struct archive *reader = archive_read_new();
        verify(reader != nullptr, "create mode reader");
        verify(archive_read_support_filter_all(reader) == ARCHIVE_OK, "mode reader filters");
        verify(archive_read_support_format_all(reader) == ARCHIVE_OK, "mode reader formats");
        verify(archive_read_open_filename(reader, QFile::encodeName(path).constData(), 65536) == ARCHIVE_OK,
               "open archive for mode inspection");
        QHash<QString, mode_t> modes;
        archive_entry *entry = nullptr;
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            modes.insert(QString::fromUtf8(archive_entry_pathname(entry)),
                         static_cast<mode_t>(archive_entry_perm(entry)));
        }
        archive_read_close(reader);
        archive_read_free(reader);
        return modes;
    };

    // A2, A6, A7 — Private sources (0700 dir, 0600 file), executable (0755), SUID (04755), SGID (02755)
    {
        const QString secIn = root.filePath(QStringLiteral("sec_in"));
        const QString privDir = secIn + QStringLiteral("/priv_dir");
        verify(QDir().mkpath(privDir), "create sec_in/priv_dir");
        writeFile(privDir + QStringLiteral("/secret.txt"), "private-data");
        writeFile(privDir + QStringLiteral("/tool.sh"), "#!/bin/sh\n");
        writeFile(privDir + QStringLiteral("/suid_file"), "suid");
        writeFile(privDir + QStringLiteral("/sgid_file"), "sgid");
        verify(::chmod(QFile::encodeName(privDir + QStringLiteral("/secret.txt")).constData(), 0600) == 0, "chmod secret 0600");
        verify(::chmod(QFile::encodeName(privDir + QStringLiteral("/tool.sh")).constData(), 0755) == 0, "chmod tool 0755");
        verify(::chmod(QFile::encodeName(privDir + QStringLiteral("/suid_file")).constData(), 04755) == 0, "chmod suid 04755");
        verify(::chmod(QFile::encodeName(privDir + QStringLiteral("/sgid_file")).constData(), 02755) == 0, "chmod sgid 02755");
        verify(::chmod(QFile::encodeName(privDir).constData(), 0700) == 0, "chmod priv_dir 0700");

        const QString privArchive = output + QStringLiteral("/private.tar.gz");
        run({QUrl::fromLocalFile(privDir)}, privArchive, ThisPcArchiveFormat::TarGzip, true);

        // A2: Output archive containing private source is owner-only 0600 and non-executable on disk
        const mode_t outMode = statModeAC(privArchive);
        verify(outMode == 0600, "A2: archive containing private source is published with 0600 mode");
        verify((outMode & 0111) == 0, "A2: published archive is non-executable");

        // A6 & A7: Archive entry metadata preserves 0700/0600/0755 without widening and strips SUID/SGID
        const auto modes = archiveEntryModes(privArchive);
        verify(modes.value(QStringLiteral("priv_dir/")) == 0700, "A6: private dir metadata stays 0700 (not widened to 0755)");
        verify(modes.value(QStringLiteral("priv_dir/secret.txt")) == 0600, "A6: private file metadata stays 0600 (not widened to 0644)");
        verify(modes.value(QStringLiteral("priv_dir/tool.sh")) == 0755, "A6: executable file metadata stays 0755");
        verify(modes.value(QStringLiteral("priv_dir/suid_file")) == 0755, "A7: SUID stripped in archive entry metadata");
        verify(modes.value(QStringLiteral("priv_dir/sgid_file")) == 0755, "A7: SGID stripped in archive entry metadata");

        // R5: Archive containing only public 0644 files also publishes at fail-safe 0600
        const QString pubFile = secIn + QStringLiteral("/public.txt");
        writeFile(pubFile, "public-data");
        verify(::chmod(QFile::encodeName(pubFile).constData(), 0644) == 0, "chmod public 0644");
        const QString pubArchive = output + QStringLiteral("/public.tar.gz");
        run({QUrl::fromLocalFile(pubFile)}, pubArchive, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(pubArchive) == 0600, "R5: public-only archive also published at fail-safe 0600");

        // R1: Archive containing only a 0600 file -> 0600
        const QString only0600 = secIn + QStringLiteral("/only0600.txt");
        writeFile(only0600, "secret-0600");
        verify(::chmod(QFile::encodeName(only0600).constData(), 0600) == 0, "chmod only0600");
        const QString arcR1 = output + QStringLiteral("/r1_0600.tar.gz");
        run({QUrl::fromLocalFile(only0600)}, arcR1, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(arcR1) == 0600, "R1: 0600 source archive published as 0600");

        // R2: Archive containing only a 0640 file -> 0600 (never widened to 0644)
        const QString only0640 = secIn + QStringLiteral("/only0640.txt");
        writeFile(only0640, "group-0640");
        verify(::chmod(QFile::encodeName(only0640).constData(), 0640) == 0, "chmod only0640");
        const QString arcR2 = output + QStringLiteral("/r2_0640.tar.gz");
        run({QUrl::fromLocalFile(only0640)}, arcR2, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(arcR2) == 0600, "R2: 0640 source archive published as 0600");

        // R3: Archive containing a 0750 directory and its contents -> 0600
        const QString dir0750 = secIn + QStringLiteral("/dir_0750");
        verify(QDir().mkpath(dir0750), "mkdir dir_0750");
        writeFile(dir0750 + QStringLiteral("/inner.txt"), "inner-0644");
        verify(::chmod(QFile::encodeName(dir0750 + QStringLiteral("/inner.txt")).constData(), 0644) == 0, "chmod inner 0644");
        verify(::chmod(QFile::encodeName(dir0750).constData(), 0750) == 0, "chmod dir_0750");
        const QString arcR3 = output + QStringLiteral("/r3_0750.tar.gz");
        run({QUrl::fromLocalFile(dir0750)}, arcR3, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(arcR3) == 0600, "R3: 0750 directory archive published as 0600");

        // R4: Archive containing a 0644 file inside a private 0700 parent -> 0600
        const QString privParent = secIn + QStringLiteral("/priv_parent_0700");
        verify(QDir().mkpath(privParent), "mkdir priv_parent_0700");
        const QString child0644 = privParent + QStringLiteral("/child0644.txt");
        writeFile(child0644, "child-under-0700");
        verify(::chmod(QFile::encodeName(child0644).constData(), 0644) == 0, "chmod child0644");
        verify(::chmod(QFile::encodeName(privParent).constData(), 0700) == 0, "chmod priv_parent_0700");
        const QString arcR4 = output + QStringLiteral("/r4_parent0700.tar.gz");
        run({QUrl::fromLocalFile(child0644)}, arcR4, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(arcR4) == 0600, "R4: 0644 file under 0700 parent published as 0600");

        // R6: Mixed public and restricted entries -> 0600
        const QString arcR6 = output + QStringLiteral("/r6_mixed.tar.gz");
        run({QUrl::fromLocalFile(pubFile), QUrl::fromLocalFile(only0640), QUrl::fromLocalFile(dir0750)},
            arcR6, ThisPcArchiveFormat::TarGzip, true);
        verify(statModeAC(arcR6) == 0600, "R6: mixed archive published as 0600");

        // R9: Collision with existing 0644 destination preserves old object and its 0644 permissions
        const QString existingSentinel = output + QStringLiteral("/r9_existing.tar.gz");
        writeFile(existingSentinel, "existing-archive-bytes");
        verify(::chmod(QFile::encodeName(existingSentinel).constData(), 0644) == 0, "chmod r9_existing 0644");
        run({QUrl::fromLocalFile(pubFile)}, existingSentinel, ThisPcArchiveFormat::TarGzip, false);
        verify(statModeAC(existingSentinel) == 0644, "R9: collision leaves existing 0644 file permissions untouched");

        // R10: ZIP, 7z, and TAR.GZ all publish at 0600 and preserve ordinary metadata where supported
        for (const auto fmt : {ThisPcArchiveFormat::Zip, ThisPcArchiveFormat::SevenZip, ThisPcArchiveFormat::TarGzip}) {
            const QString arcFmt = output + QStringLiteral("/r10_fmt") + thispcArchiveSuffix(fmt);
            run({QUrl::fromLocalFile(privDir)}, arcFmt, fmt, true);
            verify(statModeAC(arcFmt) == 0600, "R10: all archive formats publish at 0600");
            const auto fmtModes = archiveEntryModes(arcFmt);
            verify(fmtModes.value(QStringLiteral("priv_dir/secret.txt")) == 0600
                       || fmt != ThisPcArchiveFormat::TarGzip,
                   "R10: TAR.GZ preserves 0600 entry metadata");
        }
    }

    qInfo() << "archive_creation PASS:" << checks << "assertions";
    return 0;
}
