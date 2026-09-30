/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// All inputs, sentinels and output directories belong to this test's QTemporaryDir.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

struct Entry {
    QString name;
    QByteArray data = "verified archive contents\n";
    int type = AE_IFREG;
    QString link;
    bool hardlink = false;
    int permissions = -1;
};

static void archiveFixture(const QString &path, const QString &format, const QList<Entry> &entries)
{
    auto *writer = archive_write_new();
    verify(writer, "archive writer");
    if (format == "zip") { archive_write_set_format_zip(writer); archive_write_set_options(writer, "zip:compression=store"); }
    else if (format == "7z") archive_write_set_format_7zip(writer);
    else { archive_write_set_format_pax_restricted(writer); if (format == "tar.gz") archive_write_add_filter_gzip(writer); }
    verify(archive_write_open_filename(writer, QFile::encodeName(path).constData()) == ARCHIVE_OK, "open disposable archive");
    for (const auto &item : entries) {
        auto *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, item.name.toUtf8().constData());
        archive_entry_set_filetype(entry, item.type);
        archive_entry_set_perm(entry, item.permissions >= 0 ? item.permissions : (item.type == AE_IFDIR ? 0755 : 0644));
        if (!item.link.isEmpty()) {
            if (item.hardlink) archive_entry_set_hardlink_utf8(entry, item.link.toUtf8().constData());
            else archive_entry_set_symlink_utf8(entry, item.link.toUtf8().constData());
        }
        const bool data = item.type == AE_IFREG && !item.hardlink;
        archive_entry_set_size(entry, data ? item.data.size() : 0);
        verify(archive_write_header(writer, entry) == ARCHIVE_OK, "write fixture entry header");
        if (data) verify(archive_write_data(writer, item.data.constData(), item.data.size()) == item.data.size(), "write fixture contents");
        archive_entry_free(entry);
    }
    verify(archive_write_close(writer) == ARCHIVE_OK, "complete disposable archive");
    archive_write_free(writer);
}

static QByteArray contents(const QString &path)
{
    QFile file(path);
    verify(file.open(QIODevice::ReadOnly), "read disposable file");
    return file.readAll();
}

static void write(const QString &path, const QByteArray &data, bool executable = false)
{
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(data) == data.size(), "write test file");
    file.close();
    if (executable) verify(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "executable process fixture");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir root;
    verify(root.isValid(), "isolated extraction test root");
    const QString ark = QStandardPaths::findExecutable("ark");
    verify(!ark.isEmpty(), "real Ark installed");
    const QString sources = root.filePath(QStringLiteral("źródła ze spacją ; $()"));
    verify(QDir().mkpath(sources), "Unicode source directory");
    int counter = 0;
    auto destination = [&] {
        const auto path = root.filePath("destination-" + QString::number(++counter));
        verify(QDir().mkdir(path), "new disposable destination");
        return path;
    };
    auto clean = [&](const QString &path) {
        verify(QDir(path).entryList({".thispc-extract-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty(),
               "private staging cleaned after terminal state");
    };
    auto run = [&](const QString &source, const QString &target, const QString &program, bool success) {
        auto *job = new ArchiveExtractionJob(QUrl::fromLocalFile(source), QUrl::fromLocalFile(target), program);
        job->setAutoDelete(false);
        int results = 0;
        QObject::connect(job, &KJob::result, &app, [&](KJob *) { ++results; });
        job->start();
        verify(QTest::qWaitFor([&] { return results; }, 20000), "asynchronous extraction reaches terminal state");
        if (bool(job->error() == 0) != success) qWarning() << "Unexpected result:" << job->errorText();
        verify((job->error() == 0) == success, "expected extraction result");
        QTest::qWait(20);
        verify(results == 1, "exactly one terminal notification");
        const QUrl published = job->publishedUrl();
        delete job;
        clean(target);
        return published;
    };
    const QByteArray payload("verified archive contents\n");
    QString zip;
    for (const QString format : {QString("zip"), QString("7z"), QString("tar.gz"), QString("tar")}) {
        const QString source = sources + "/-Żółw # ; $(touch injected)." + format;
        archiveFixture(source, format, {{QStringLiteral("żółw # ; $.txt")}});
        if (format == "zip") zip = source;
        verify(thispcCanExtractArchive(QUrl::fromLocalFile(source), false), "content-based archive candidate");
        const QString target = destination();
        const auto published = run(source, target, ark, true);
        verify(published == QUrl::fromLocalFile(target + QStringLiteral("/żółw # ; $.txt")), "single-file autosubfolder semantics");
        verify(contents(published.toLocalFile()) == payload, "real Ark produces exact Unicode filename and bytes");
        verify(!QFileInfo::exists(sources + "/injected"), "no shell expansion");
        run(source, target, ark, false);
        verify(contents(published.toLocalFile()) == payload, "collision preserves original bytes");
    }
    const QString multi = sources + "/multiple.tar.gz";
    archiveFixture(multi, "tar.gz", {{"one.txt"}, {"nested/two.txt"}, {"empty", {}, AE_IFDIR}});
    const QString multiTarget = destination();
    run(multi, multiTarget, ark, true);
    verify(contents(multiTarget + "/multiple/one.txt") == payload
           && contents(multiTarget + "/multiple/nested/two.txt") == payload
           && QFileInfo(multiTarget + "/multiple/empty").isDir(), "multiple roots publish a single named folder");
    const QString folder = sources + "/folder.zip";
    archiveFixture(folder, "zip", {{"folder", {}, AE_IFDIR}, {"folder/file.txt"}});
    const QString folderTarget = destination();
    run(folder, folderTarget, ark, true);
    verify(contents(folderTarget + "/folder/file.txt") == payload, "single-root archive retains its folder without double nesting");
    write(folderTarget + "/folder/sentinel", "keep");
    run(folder, folderTarget, ark, false);
    verify(contents(folderTarget + "/folder/sentinel") == "keep", "existing directories are never merged");

    const QString rootMode = sources + "/root-mode.tar";
    archiveFixture(rootMode, "tar", {{"./", {}, AE_IFDIR, {}, false, 0777}, {"one"}, {"two"}});
    const QString modeTarget = destination();
    run(rootMode, modeTarget, ark, true);
    struct stat publishedMode {};
    verify(::stat(QFile::encodeName(modeTarget + "/root-mode").constData(), &publishedMode) == 0
           && (publishedMode.st_mode & 07777) == 0700, "published container root always remains private");
    const QString inaccessible = destination();
    verify(::chmod(QFile::encodeName(inaccessible).constData(), 0500) == 0, "read-only disposable target");
    run(zip, inaccessible, ark, false);
    verify(::chmod(QFile::encodeName(inaccessible).constData(), 0700) == 0, "restore only disposable target mode");

    const QString disguised = sources + "/actually-a-zip.txt";
    verify(QFile::copy(zip, disguised), "misleading extension fixture");
    run(disguised, destination(), ark, true);
    const QString gz = sources + "/not-tar.tar.gz";
    gzFile gzip = gzopen(QFile::encodeName(gz).constData(), "wb");
    verify(gzip && gzwrite(gzip, "ordinary gzip data", 18) == 18 && gzclose(gzip) == Z_OK, "ordinary gzip fixture");
    verify(!thispcIsArchiveCandidate(QUrl::fromLocalFile(gz), false), "ordinary gzip with tar.gz extension rejected");
    run(gz, destination(), ark, false);
    const QString broken = sources + "/broken.zip";
    write(broken, QByteArray("PK\3\4", 4) + QByteArray(50, '\0'));
    run(broken, destination(), ark, false);
    const QString truncated = sources + "/truncated.zip";
    write(truncated, contents(zip).left(50));
    run(truncated, destination(), ark, false);
    run(zip, destination(), root.filePath("absent-ark"), false);
    const QString badCrc = sources + "/bad-crc.zip";
    QByteArray damaged = contents(zip);
    const auto payloadIndex = damaged.indexOf(payload);
    verify(payloadIndex >= 0, "stored ZIP payload located for CRC test");
    damaged[payloadIndex] = 'X';
    write(badCrc, damaged);
    run(badCrc, destination(), ark, false);
    for (const QString &format : {QString("zip"), QString("7z")}) {
        const QString encrypted = sources + "/encrypted." + format;
        QProcess create;
        create.setWorkingDirectory(sources);
        const QStringList arguments = format == "zip"
            ? QStringList{"-q", "-Psecret", encrypted, zip}
            : QStringList{"a", "-psecret", "-mhe=on", encrypted, zip};
        create.start(QStandardPaths::findExecutable(format == "zip" ? "zip" : "7z"), arguments);
        verify(create.waitForFinished(10000) && create.exitCode() == 0, "encrypted archive fixture");
        run(encrypted, destination(), ark, false);
    }

    const QString sentinel = root.filePath("sentinel");
    write(sentinel, "untouched");
    const QList<QList<Entry>> hostile {
        {{"../sentinel"}}, {{sentinel}}, {{"C:/sentinel"}}, {{"x\\..\\sentinel"}},
        {{"a/../../sentinel"}}, {{"a", {}, AE_IFLNK, sentinel}, {"a/child"}},
        {{"link", {}, AE_IFLNK, "missing"}}, {{"hard", {}, AE_IFREG, sentinel, true}},
        {{"pipe", {}, AE_IFIFO}}, {{"same"}, {"same"}}, {{"a"}, {"a/file"}},
        {{"a/file"}, {"a"}}, {{"bad\nname"}}
    };
    for (const auto &entries : hostile) {
        const QString bad = sources + "/hostile-" + QString::number(++counter) + ".tar";
        archiveFixture(bad, "tar", entries);
        const QString target = destination();
        run(bad, target, ark, false);
        verify(QDir(target).isEmpty(), "unsafe archive publishes nothing");
        verify(contents(sentinel) == "untouched", "outside sentinel unchanged");
    }
    for (bool dangling : {false, true}) {
        const QString target = destination();
        const QString link = target + QStringLiteral("/żółw # ; $.txt");
        verify(QFile::link(dangling ? root.filePath("missing") : sentinel, link), "target symlink fixture");
        run(zip, target, ark, false);
        verify(QFileInfo(link).isSymLink() && contents(sentinel) == "untouched", "existing symlinks never followed or replaced");
    }
    const QString fake = root.filePath("fake ark");
    const QByteArray python("#!/usr/bin/python3\nimport os,sys,time,pathlib,signal\nd=pathlib.Path(sys.argv[sys.argv.index('--destination')+1])\n");
    write(fake, python + "sys.exit(0)\n", true);
    run(zip, destination(), fake, false); // Zero, but no extracted files.
    write(fake, python + "sys.exit(19)\n", true);
    run(zip, destination(), fake, false);
    write(fake, python + "os.kill(os.getpid(),signal.SIGKILL)\n", true);
    run(zip, destination(), fake, false);
    write(fake, python + "(d/'żółw # ; $.txt').write_bytes(b'X'*25)\n", true);
    run(zip, destination(), fake, false);
    // A backend trying an absolute write must be stopped by the kernel even
    // after preflight accepted the input. The sentinel belongs to this test.
    write(fake, python + "pathlib.Path(r'" + QFile::encodeName(sentinel) + "').write_text('corrupted')\n", true);
    run(zip, destination(), fake, false);
    verify(contents(sentinel) == "untouched", "Landlock blocks backend writes outside staging");

    // Late collisions must be rejected by atomic no-replace publication, even
    // when the target did not exist during the initial check.
    const QByteArray validOutput = "(d/'żółw # ; $.txt').write_bytes(b'verified archive contents\\n')\n";
    const QString lateTarget = destination();
    write(fake, python + validOutput
        + "s=pathlib.Path(os.environ['XDG_RUNTIME_DIR'])\n(s/'ready').touch()\nwhile not (s/'go').exists(): time.sleep(.01)\n", true);
    QTimer collision;
    collision.setInterval(5);
    bool raced = false;
    QObject::connect(&collision, &QTimer::timeout, &app, [&] {
        QDirIterator it(lateTarget, {"ready"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        if (!it.hasNext()) return;
        const QString ready = it.next();
        write(lateTarget + QStringLiteral("/żółw # ; $.txt"), "late existing data");
        write(QFileInfo(ready).absolutePath() + "/go", "");
        raced = true;
        collision.stop();
    });
    collision.start();
    run(zip, lateTarget, fake, false);
    collision.stop();
    verify(raced && contents(lateTarget + QStringLiteral("/żółw # ; $.txt")) == "late existing data",
           "late collision preserves all existing bytes");

    // Rename a chosen directory while Ark works, then replace its old name
    // with a symlink. Neither publication nor cleanup may follow the replacement.
    const QString movingTarget = destination();
    const QString movedTarget = movingTarget + "-moved";
    const QString replacement = destination();
    write(replacement + "/sentinel", "keep replacement");
    write(fake, python + validOutput
        + "s=pathlib.Path(os.environ['XDG_RUNTIME_DIR'])\n(s/'ready').touch()\nwhile not (s/'go').exists(): time.sleep(.01)\n", true);
    QTimer rename;
    rename.setInterval(5);
    bool renamed = false;
    QObject::connect(&rename, &QTimer::timeout, &app, [&] {
        QDirIterator it(movingTarget, {"ready"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        if (!it.hasNext()) return;
        const QString relative = QDir(movingTarget).relativeFilePath(it.next());
        verify(QDir().rename(movingTarget, movedTarget), "rename disposable destination during extraction");
        verify(QFile::link(replacement, movingTarget), "replace old destination with symlink");
        write(QFileInfo(QDir(movedTarget).filePath(relative)).absolutePath() + "/go", "");
        renamed = true;
        rename.stop();
    });
    rename.start();
    run(zip, movingTarget, fake, false);
    rename.stop();
    clean(movedTarget);
    verify(renamed && QDir(movedTarget).isEmpty()
           && contents(replacement + "/sentinel") == "keep replacement",
           "renamed destination fails safely and cleanup remains anchored");

    // Publish readiness only after the helper has started. Both processes wait
    // for a signal, so cancellation never races a naturally completing extract.
    // The alarm is only a fail-safe if the test aborts before killing the job.
    write(fake, python + "signal.alarm(20)\nchild=os.fork()\nif child==0:\n"
        " signal.alarm(20)\n (d/'child-ready').write_text(str(os.getpid()))\n signal.pause()\nelse:\n"
        " while not (d/'child-ready').exists(): time.sleep(.01)\n"
        " (d/'partial').write_text(str(os.getpid()))\n signal.pause()\n", true);
    const QString cancelledTarget = destination();
    auto *cancelled = new ArchiveExtractionJob(QUrl::fromLocalFile(zip), QUrl::fromLocalFile(cancelledTarget), fake);
    cancelled->setAutoDelete(false);
    int cancelledResults = 0;
    QObject::connect(cancelled, &KJob::result, &app, [&](KJob *) { ++cancelledResults; });
    cancelled->start();
    QString marker;
    verify(QTest::qWaitFor([&] {
        QDirIterator it(cancelledTarget, {"partial"}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            const QString candidate = it.next();
            if (contents(candidate).trimmed().toLongLong() > 0) marker = candidate;
        }
        return !marker.isEmpty();
    }, 5000), "real asynchronous backend started before cancellation");
    const auto pid = contents(marker).trimmed().toLongLong();
    verify(cancelled->kill(KJob::EmitResult), "running extraction accepts cancellation");
    verify(cancelledResults == 1 && cancelled->error() == KJob::KilledJobError, "cancel is a distinct terminal result");
    delete cancelled;
    clean(cancelledTarget);
    verify(QDir(cancelledTarget).isEmpty(), "cancelled extraction publishes no partial files");
    verify(::kill(pid, 0) == -1 && errno == ESRCH, "Ark process reaped before destruction completes");
    run(zip, cancelledTarget, ark, true);
    verify(contents(cancelledTarget + QStringLiteral("/żółw # ; $.txt")) == "verified archive contents\n",
           "same archive and destination can be retried successfully after cancellation");
    const QString closingTarget = destination();
    // Destroy while work is queued, as when the application closes.
    auto *closing = new ArchiveExtractionJob(QUrl::fromLocalFile(zip), QUrl::fromLocalFile(closingTarget), fake);
    closing->start();
    delete closing;
    clean(closingTarget);
    verify(QDir(closingTarget).isEmpty(), "owner destruction cancels pending work safely");
    qInfo("PASS: %d archive extraction assertions; real Ark, collisions, hostile entries, process lifecycle and confinement", checks);
}
