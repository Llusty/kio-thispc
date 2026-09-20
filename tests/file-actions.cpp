// Every real operation is restricted to this suite's disposable directory.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-actions-test");
    QCoreApplication::setApplicationName("actions-test");
    interceptFileJobs = false;
    // KIO Trash requires data on a filesystem with a writable trash. Keep
    // both the fixture and isolated XDG_DATA_HOME on the project filesystem.
    QTemporaryDir files(QString::fromLocal8Bit(qgetenv("THISPC_TEST_FILES")) + "/actions-XXXXXX");
    verify(files.isValid(), "temporary directory");
    const QString a = files.filePath("A"), b = files.filePath("B");
    verify(QDir().mkpath(a) && QDir().mkpath(b), "disposable operation roots");
    const QUrl rootA = QUrl::fromLocalFile(a), rootB = QUrl::fromLocalFile(b);
    auto createFixture = [&](const QString &path, const QByteArray &data = {}) {
        QFile file(path);
        verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && file.write(data) == data.size(), "collision fixture created");
    };
    auto fixtureData = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    createFixture(a + "/report.tar.gz");
    createFixture(a + "/report (1).tar.gz");
    createFixture(a + "/report (3).tar.gz");
    createFixture(a + "/.gitignore");
    const QString reportSuggestion =
        FileActions::suggestedAvailableName(rootA, "report.tar.gz", false);
    verify(reportSuggestion == "report (2).tar.gz",
           "first numbering gap preserves a multi-part extension");
    verify(FileActions::suggestedAvailableName(rootA, ".gitignore", false)
               == ".gitignore (1)", "hidden name is not treated as an extension");
    verify(FileActions::suggestedAvailableName(rootB, QString::fromUtf8("żółw.txt"), false)
               == QString::fromUtf8("żółw.txt"), "free Unicode name remains unchanged");
    verify(QDir().mkdir(a + "/Nowy folder") && QDir().mkdir(a + "/Nowy folder (1)"),
           "folder collision fixtures created");
    verify(FileActions::suggestedAvailableName(rootA, "Nowy folder", true)
               == "Nowy folder (2)", "folder numbering finds the first free name");
    QWidget parent;
    QAction undoAction(&parent), redoAction(&parent);
    UndoController undo(&parent, [](bool) {}, [] {}, [](const QString &, int) {});
    undo.setActions(&undoAction, &redoAction);
    int completed = 0, lastError = 0;
    bool lastClear = false, interactive = true;
    bool lastNativeCopy = false, lastNativeMove = false;
    bool lastNativeTree = false, lastTreeUsedKio = false, lastSuspendable = false;
    bool pauseNextTree = false;
    QString lastMessage, lastTitle;
    QPointer<LocalTransferJob> observedTree;
    FileActions actions(&parent, &undo, [&](KJob *job, const QString &message, bool clear, const QString &title,
                                          const FileActions::RefreshViews &refreshViews) {
        verify(!refreshViews, "ordinary file operations keep the default view refresh");
        lastMessage = message;
        lastTitle = title;
        lastClear = clear;
        lastNativeCopy = qobject_cast<LocalFileCopyJob *>(job) != nullptr;
        lastNativeMove = qobject_cast<LocalFileMoveJob *>(job) != nullptr;
        observedTree = qobject_cast<LocalTransferJob *>(job);
        lastNativeTree = observedTree != nullptr;
        if (observedTree && pauseNextTree) {
            pauseNextTree = false;
            verify(observedTree->suspend(), "tree job can pause before planning completes");
        }
        if (auto *copy = qobject_cast<KIO::CopyJob *>(job)) {
            const bool safeCreation = title == "Creating file";
            verify(safeCreation ? copy->uiDelegate() == nullptr
                                : interactive ? copy->uiDelegate() != nullptr : copy->uiDelegate() == nullptr,
                   "operation has the expected delegate");
            if (interactive && !safeCreation)
                verify(!copy->uiDelegate()->isAutoErrorHandlingEnabled(), "no duplicate automatic error UI");
        }
        QObject::connect(job, &KJob::result, &parent, [&](KJob *done) {
            if (auto *tree = qobject_cast<LocalTransferJob *>(done)) lastTreeUsedKio = tree->usedKio();
            lastSuspendable = done->capabilities().testFlag(KJob::Suspendable);
            lastError = done->error();
            ++completed;
        });
    });
    auto waitJob = [&](int count, bool success = true) {
        verify(QTest::qWaitFor([&] { return completed == count; }, 10000), "file job completes");
        verify(success ? lastError == 0 : lastError != 0, "expected file job result");
    };
    auto answerName = [&](const QString &name) {
        QTimer::singleShot(0, &parent, [name] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            verify(dialog != nullptr, "name dialog opened");
            dialog->setTextValue(name);
            dialog->accept();
        });
    };
    auto undoRedo = [&](const std::function<bool()> &undone, const std::function<bool()> &redone) {
        verify(undoAction.isEnabled(), "Undo action available");
        undo.undo();
        verify(QTest::qWaitFor([&] { return !undo.m_busy && undone(); }, 10000), "real KIO Undo completes");
        verify(redoAction.isEnabled(), "Redo action available");
        undo.redo();
        verify(QTest::qWaitFor([&] { return !undo.m_busy && redone(); }, 10000), "real KIO Redo completes");
    };
    createFixture(a + "/default.txt", "existing");
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog && dialog->textValue() == "default (1).txt",
               "occupied default is suggested before the dialog is accepted");
        createFixture(a + "/default (1).txt", "race winner");
        dialog->accept();
        QTimer::singleShot(0, &parent, [] {
            auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            verify(warning != nullptr, "race collision is reported without dispatch");
            warning->accept();
        });
    });
    actions.createNewFile(rootA, "default.txt", QByteArray("must not overwrite"));
    verify(completed == 0 && fixtureData(a + "/default.txt") == "existing"
               && fixtureData(a + "/default (1).txt") == "race winner",
           "pre-dispatch race preserves both existing files and starts no job");
    answerName("created.txt");
    actions.createNewFile(rootA, "default.txt", QByteArray("original payload"));
    waitJob(1);
    verify(QFile::exists(a + "/created.txt"), "file created in requested root");
    // Undoing Put may ask before removing the newly-created file; test
    // rename and move history below without relying on that confirmation.
    answerName("folder");
    actions.createNewFolder(rootA);
    waitJob(2);
    verify(QFileInfo(a + "/folder").isDir(), "folder created");
    answerName("renamed.txt");
    actions.renameSelected({QUrl::fromLocalFile(a + "/created.txt")}, "created.txt");
    waitJob(3);
    verify(QFile::exists(a + "/renamed.txt") && !QFile::exists(a + "/created.txt"), "rename performed");
    undoRedo([&] { return QFile::exists(a + "/created.txt"); },
             [&] { return QFile::exists(a + "/renamed.txt"); });
    const QUrl source = QUrl::fromLocalFile(a + "/renamed.txt");
    actions.transfer({source}, rootB, Qt::CopyAction);
    waitJob(4);
    verify(QFile::exists(a + "/renamed.txt") && QFile::exists(b + "/renamed.txt"), "copy preserves source");
    verify(lastNativeCopy, "supported single local copy uses native engine");
    verify(!lastClear, "drag copy does not clear clipboard");

    // Drive the actual KIO conflict dialog through its public actions.
    for (bool cancel : {false, true}) {
        bool conflictSeen = false;
        QTimer conflict;
        conflict.setInterval(10);
        QObject::connect(&conflict, &QTimer::timeout, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (auto *dialog = qobject_cast<KIO::RenameDialog *>(widget); dialog && dialog->isVisible()) {
                    conflictSeen = true;
                    conflict.stop();
                    if (cancel) dialog->cancelPressed();
                    else dialog->overwritePressed();
                    break;
                }
            }
        });
        conflict.start();
        const int expected = completed + 1;
        actions.transfer({source}, rootB, Qt::CopyAction);
        if (cancel) verify(completed == expected - 1, "cancel conflict dispatches no transfer");
        else {
            waitJob(expected);
            verify(lastNativeCopy, "overwrite uses the native copy engine");
            verify(!undoAction.isEnabled() && !redoAction.isEnabled(),
                   "irreversible overwrite blocks earlier unsafe history");
        }
        verify(conflictSeen, "KIO conflict UI remains interactive");
        verify(QFile::exists(a + "/renamed.txt") && QFile::exists(b + "/renamed.txt"), "conflict preserves expected files");
    }
    auto *mime = new QMimeData;
    mime->setUrls({source});
    mime->setData("application/x-kde-cutselection", "1");
    app.clipboard()->setMimeData(mime);
    actions.pasteClipboardInto(QUrl::fromLocalFile(a + "/folder"));
    waitJob(6);
    verify(lastClear, "cut requests clipboard cleanup only on success");
    verify(!QFile::exists(a + "/renamed.txt") && QFile::exists(a + "/folder/renamed.txt"), "clipboard cut moves source");
    verify(lastNativeMove, "supported single local move uses native engine");
    undoRedo([&] { return QFile::exists(a + "/renamed.txt"); },
             [&] { return QFile::exists(a + "/folder/renamed.txt"); });
    QFile payload(a + "/folder/renamed.txt");
    verify(payload.open(QIODevice::ReadOnly) && payload.readAll() == "original payload", "file contents survive move and Undo/Redo");
    interactive = false;
    const QString trashed = b + "/renamed.txt";
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        verify(dialog != nullptr, "Trash confirmation opened");
        dialog->button(QMessageBox::Yes)->click();
    });
    actions.trashSelected({QUrl::fromLocalFile(trashed)});
    waitJob(7);
    verify(!QFile::exists(trashed), "disposable file moved to Trash");
    undoRedo([&] { return QFile::exists(trashed); }, [&] { return !QFile::exists(trashed); });
    undo.undo();
    verify(QTest::qWaitFor([&] { return !undo.m_busy && QFile::exists(trashed); }, 10000),
           "restore disposable file from Trash before cleanup");

    const QString nativeCopySource = a + "/native-copy.txt";
    QFile nativeFixture(nativeCopySource);
    verify(nativeFixture.open(QIODevice::WriteOnly)
               && nativeFixture.write("native copy history") == 19,
           "native copy Undo fixture is created");
    nativeFixture.close();
    actions.transfer({QUrl::fromLocalFile(nativeCopySource)}, rootB, Qt::CopyAction);
    waitJob(8);
    const QString nativeCopyDestination = b + "/native-copy.txt";
    verify(lastNativeCopy && QFile::exists(nativeCopyDestination),
           "native copy completes before history test");
    undoRedo([&] { return !QFile::exists(nativeCopyDestination); },
             [&] { return QFile::exists(nativeCopyDestination); });

    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    auto *cutMime = new QMimeData;
    cutMime->setUrls({QUrl::fromLocalFile(nativeCopySource)});
    cutMime->setData("application/x-kde-cutselection", "1");
    app.clipboard()->setMimeData(cutMime);
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = qobject_cast<KIO::RenameDialog *>(QApplication::activeModalWidget());
        verify(dialog != nullptr, "native skip uses the KDE conflict dialog");
        dialog->skipPressed();
    });
    const int beforeSkip = completed;
    actions.pasteClipboardInto(rootB);
    verify(completed == beforeSkip && QFile::exists(nativeCopySource)
               && readFile(nativeCopyDestination) == "native copy history"
               && app.clipboard()->mimeData()->data("application/x-kde-cutselection") == "1",
           "skip retains both files and the cut clipboard");

    QUrl renamedDestination;
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = qobject_cast<KIO::RenameDialog *>(QApplication::activeModalWidget());
        verify(dialog != nullptr, "native rename uses the KDE conflict dialog");
        dialog->suggestNewNamePressed();
        renamedDestination = dialog->newDestUrl();
        dialog->renamePressed();
    });
    actions.transfer({QUrl::fromLocalFile(nativeCopySource)}, rootB, Qt::CopyAction);
    waitJob(beforeSkip + 1);
    verify(lastNativeCopy && renamedDestination != QUrl::fromLocalFile(nativeCopyDestination)
               && readFile(renamedDestination.toLocalFile()) == "native copy history"
               && readFile(nativeCopyDestination) == "native copy history",
           "rename publishes natively at the suggested name and preserves the conflict");
    undoRedo([&] { return !QFile::exists(renamedDestination.toLocalFile()); },
             [&] { return QFile::exists(renamedDestination.toLocalFile()); });

    verify(QDir().mkpath(a + "/batch/nested"), "batch source directory exists");
    QFile batchFile(a + "/batch/nested/data");
    verify(batchFile.open(QIODevice::WriteOnly) && batchFile.write("batch data") == 10,
           "batch file is created");
    batchFile.close();
    verify(::symlink("nested/data", QFile::encodeName(a + "/batch/link").constData()) == 0,
           "batch contains a relative symbolic link");
    const QList<QUrl> batchSources{QUrl::fromLocalFile(a + "/batch"), QUrl::fromLocalFile(nativeCopySource)};
    const QString batchDestination = files.filePath("batch-target");
    verify(QDir().mkdir(batchDestination), "batch target directory exists");
    int expectedBatch = completed + 1;
    actions.transfer(batchSources, QUrl::fromLocalFile(batchDestination), Qt::CopyAction);
    waitJob(expectedBatch);
    verify(lastNativeTree && !lastTreeUsedKio && lastSuspendable
               && readFile(batchDestination + "/batch/nested/data") == "batch data"
               && readLocalLink(batchDestination + "/batch/link") == "nested/data",
           "FileActions routes directories, multiple sources and links through native execution");
    undoRedo([&] { return !QFileInfo::exists(batchDestination + "/batch")
                           && !QFileInfo::exists(batchDestination + "/native-copy.txt"); },
             [&] { return QFileInfo::exists(batchDestination + "/batch/nested/data")
                           && QFileInfo::exists(batchDestination + "/native-copy.txt"); });

    bool fallbackConflictSeen = false;
    QTimer fallbackDialog;
    fallbackDialog.setInterval(10);
    QObject::connect(&fallbackDialog, &QTimer::timeout, [&] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<KIO::RenameDialog *>(widget); dialog && dialog->isVisible()) {
                fallbackConflictSeen = true;
                dialog->overwriteAllPressed();
            }
        }
    });
    fallbackDialog.start();
    pauseNextTree = true;
    expectedBatch = completed + 1;
    actions.transfer(batchSources, QUrl::fromLocalFile(batchDestination), Qt::CopyAction);
    QTest::qWait(200);
    verify(observedTree && observedTree->isSuspended() && !fallbackConflictSeen
               && completed == expectedBatch - 1,
           "paused planning does not begin the KIO fallback");
    verify(observedTree->resume(), "paused fallback plan resumes");
    waitJob(expectedBatch);
    fallbackDialog.stop();
    verify(fallbackConflictSeen && lastTreeUsedKio && !lastSuspendable
               && readFile(batchDestination + "/batch/nested/data") == "batch data",
           "unresolved tree conflicts keep KIO dialogs and never advertise exact pause");
    verify(!actions.startNativeSingleFileTransfer(
               {QUrl(QStringLiteral("sftp://example.invalid/file"))}, rootB,
               false, false, {}, {}), "remote URLs remain on the existing KIO route");

    // Template operations use the real KIO copy/Undo path with a confirmed
    // destination name, never the clipboard or in-memory text conversion.
    interactive = true;
    const QString templatePath = files.filePath("source #ż.odt");
    const QByteArray templateData = QByteArray::fromHex("504b030400ff00c5bcc3b30a");
    QFile templateFile(templatePath);
    verify(templateFile.open(QIODevice::WriteOnly)
               && templateFile.write(templateData) == templateData.size(), "binary template fixture");
    templateFile.close();
    const QUrl templateSource = QUrl::fromLocalFile(templatePath);
    for (const auto &directory : {rootA, rootB}) {
        const int next = completed + 1;
        answerName("  result #ż.odt  ");
        actions.createFromTemplate(directory, templateSource);
        waitJob(next);
        const QString destination = directory.toLocalFile() + "/result #ż.odt";
        verify(readFile(destination) == templateData && readFile(templatePath) == templateData,
               "template copy preserves binary contents and source at a confirmed name");
        verify(!lastClear && lastMessage == "File created" && lastTitle == "Creating file"
                   && !lastNativeCopy && !lastNativeTree,
               "template operation uses the shared watcher, creation status and KIO copy history");
        verify(app.clipboard()->mimeData()->data("application/x-kde-cutselection") == "1",
               "template creation leaves cut clipboard untouched");
        undoRedo([&] { return !QFile::exists(destination) && readFile(templatePath) == templateData; },
                 [&] { return readFile(destination) == templateData && readFile(templatePath) == templateData; });
    }
    const int beforeRejected = completed;
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog != nullptr, "template name dialog can be cancelled");
        dialog->reject();
    });
    actions.createFromTemplate(rootA, templateSource);
    verify(completed == beforeRejected, "cancelled template name starts no operation");
    for (const QString &invalid : {QString("   "), QString("."), QString(".."), QString("../escape")}) {
        answerName(invalid);
        QTimer warning;
        warning.setInterval(10);
        bool warned = false;
        QObject::connect(&warning, &QTimer::timeout, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                warned = true;
                box->accept();
                warning.stop();
            }
        });
        warning.start();
        actions.createFromTemplate(rootA, templateSource);
        verify(warned && completed == beforeRejected, "invalid template name warns without a file operation");
    }
    verify(!QFile::exists(files.filePath("escape")), "name validation prevents escape from the target directory");

    // A manually entered occupied template name is refused before dispatch.
    bool templateConflictSeen = false;
    answerName("result #ż.odt");
    QTimer templateWarning;
    templateWarning.setInterval(10);
    QObject::connect(&templateWarning, &QTimer::timeout, [&] {
        if (auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            templateConflictSeen = true;
            templateWarning.stop();
            warning->accept();
        }
    });
    templateWarning.start();
    actions.createFromTemplate(rootA, templateSource);
    verify(templateConflictSeen && completed == beforeRejected
               && readFile(a + "/result #ż.odt") == templateData,
           "template collision starts no job and preserves the existing file");

    answerName("missing-template-result.odt");
    const int next = completed + 1;
    actions.createFromTemplate(rootA, QUrl::fromLocalFile(files.filePath("missing-template.odt")));
    waitJob(next, false);
    verify(lastMessage == "File created" && lastTitle == "Creating file"
               && !QFile::exists(a + "/missing-template-result.odt"),
           "stale missing template reports its error through the shared operation watcher");

    // Remote Stage 2 uses an injected, fully isolated directory backend. No
    // network worker or user location is touched by these async checks.
    interceptFileJobs = true;
    QList<FileActions::DirectorySnapshotCallback> remoteSnapshots;
    const auto startMockSnapshot =
        [&](const QUrl &, QObject *, FileActions::DirectorySnapshotCallback callback) -> KJob * {
            remoteSnapshots.append(std::move(callback));
            return nullptr;
        };
    FileActions remoteActions(
        &parent, &undo,
        [&](KJob *, const QString &, bool, const QString &,
            const FileActions::RefreshViews &) {
            verify(false, "mock remote tests intercept every final dispatch");
        }, {}, startMockSnapshot);
    const QUrl remoteDirectory(QStringLiteral("mock://host/share"));

    dispatch = {};
    remoteActions.createNewFile(
        remoteDirectory, QString::fromUtf8("żółw.tar.gz"), QByteArrayLiteral("remote"));
    verify(remoteSnapshots.size() == 1 && !QApplication::activeModalWidget(),
           "remote suggestion starts asynchronously before opening a dialog");
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog && dialog->textValue() == QString::fromUtf8("żółw (2).tar.gz"),
               "remote Unicode suggestion preserves the complete suffix and first gap");
        dialog->accept();
    });
    remoteSnapshots.takeFirst()({0, {}, {
        QString::fromUtf8("żółw.tar.gz"), QString::fromUtf8("żółw (1).tar.gz")}});
    verify(remoteSnapshots.size() == 1 && dispatch.kind.isEmpty(),
           "accepting a remote name starts a second snapshot before dispatch");
    QTimer::singleShot(0, &parent, [] {
        auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        verify(warning != nullptr, "remote post-dialog collision is reported");
        warning->accept();
    });
    remoteSnapshots.takeFirst()({0, {}, {QString::fromUtf8("żółw (2).tar.gz")}});
    verify(dispatch.kind.isEmpty(),
           "remote race collision fails closed without dispatching a create job");

    remoteActions.createNewFolder(remoteDirectory);
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog && dialog->textValue() == "New folder (2)",
               "remote folder suggestion finds the first numbering gap");
        dialog->setTextValue(QString::fromUtf8("ręczny folder"));
        dialog->accept();
    });
    remoteSnapshots.takeFirst()({0, {}, {"New folder", "New folder (1)"}});
    verify(remoteSnapshots.size() == 1, "remote folder is rechecked after manual editing");
    remoteSnapshots.takeFirst()({0, {}, {}});
    verify(dispatch.kind == "mkdir"
               && dispatch.destination == childUrlWithName(
                   remoteDirectory, QString::fromUtf8("ręczny folder")),
           "free manually edited remote folder name is dispatched at the captured URL");

    dispatch = {};
    remoteActions.createFromTemplate(remoteDirectory, templateSource);
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog && dialog->textValue() == QString::fromUtf8("source #ż.odt"),
               "remote template keeps its Unicode source name");
        dialog->accept();
    });
    remoteSnapshots.takeFirst()({0, {}, {}});
    remoteSnapshots.takeFirst()({0, {}, {}});
    verify(dispatch.kind == "template" && dispatch.sources == QList<QUrl>{templateSource}
               && dispatch.destination == childUrlWithName(
                   remoteDirectory, QString::fromUtf8("source #ż.odt")),
           "remote template uses the no-overwrite copyAs dispatch path");

    dispatch = {};
    remoteActions.createNewFile(remoteDirectory, "blocked.txt", {});
    QTimer::singleShot(0, &parent, [] {
        auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        verify(warning && warning->text().contains("No item was created"),
               "remote listing failure has explicit fail-closed UI");
        warning->accept();
    });
    remoteSnapshots.takeFirst()({KIO::ERR_CANNOT_OPEN_FOR_READING, "mock listing failure", {}});
    verify(dispatch.kind.isEmpty() && remoteSnapshots.isEmpty(),
           "remote listing error is never interpreted as an available name");

    remoteActions.createNewFile(remoteDirectory, "stale-a.txt", {});
    const auto staleSnapshot = remoteSnapshots.takeFirst();
    remoteActions.createNewFile(remoteDirectory, "current-b.txt", {});
    staleSnapshot({0, {}, {}});
    verify(remoteSnapshots.size() == 1 && !QApplication::activeModalWidget(),
           "superseded remote callback cannot open a stale dialog");
    QTimer::singleShot(0, &parent, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        verify(dialog && dialog->textValue() == "current-b.txt",
               "latest remote request owns the dialog");
        dialog->reject();
    });
    remoteSnapshots.takeFirst()({0, {}, {}});
    verify(remoteSnapshots.isEmpty() && dispatch.kind.isEmpty(),
           "cancelling the current remote dialog starts no final check or job");

    QList<FileActions::DirectorySnapshotCallback> closingSnapshots;
    QWidget *closingParent = new QWidget;
    auto *closingActions = new FileActions(
        closingParent, nullptr,
        [](KJob *, const QString &, bool, const QString &,
           const FileActions::RefreshViews &) {}, {},
        [&](const QUrl &, QObject *, FileActions::DirectorySnapshotCallback callback) -> KJob * {
            closingSnapshots.append(std::move(callback));
            return nullptr;
        });
    closingActions->createNewFile(remoteDirectory, "closed.txt", {});
    verify(closingSnapshots.size() == 1, "closing-window remote callback is pending");
    delete closingParent;
    closingSnapshots.takeFirst()({0, {}, {}});
    verify(!QApplication::activeModalWidget(),
           "callback after window destruction is ignored safely");
    interceptFileJobs = false;

    qInfo("PASS: %d FileActions assertions; native local copy/move, KIO fallbacks, conflicts, Undo/Redo", checks);
}
