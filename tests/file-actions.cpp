/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

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
    bool lastNativeCopy = false, lastNativeMove = false, lastNativeDirMove = false;
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
        lastNativeDirMove = qobject_cast<LocalDirectoryMoveJob *>(job) != nullptr;
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
    actions.renameTo(QUrl::fromLocalFile(a + "/created.txt"), "renamed.txt");
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

    // =========================================================================
    // Stage 5.3.2G.3A.1: Move Routing & Safety Containment Scenarios (A - J)
    // =========================================================================

    QString lastFenceMessage;
    actions.setShowRecoveryFence([&lastFenceMessage](const QString &msg) {
        lastFenceMessage = msg;
    });

    // Scenario A: Ctrl+X / Ctrl+V of a single regular local file with transfer queue enabled
    {
        const QString srcA = files.filePath("queue-single-src.txt");
        const QString dstA = b + "/queue-single-src.txt";
        createFixture(srcA, "queue-move-payload");
        auto *cutMimeA = new QMimeData;
        cutMimeA->setUrls({QUrl::fromLocalFile(srcA)});
        cutMimeA->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeA);

        TransferQueueController queueCtl(&parent);
        actions.setQueueController(&queueCtl);
        setTransferQueueProductionDispatchConnected(true);

        actions.pasteClipboardInto(rootB);
        verify(queueCtl.snapshot().tasks.size() == 1,
               "Scenario A: single regular file move enqueued to production queue");
        verify(QTest::qWaitFor([&queueCtl] {
                   return queueCtl.snapshot().tasks.front().state == TransferTaskState::Completed;
               }, 5000),
               "Scenario A: production queue task completes");
        verify(!QFile::exists(srcA) && readFile(dstA) == "queue-move-payload",
               "Scenario A: single regular local file moved safely via queue");

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // Scenario B: Ctrl+X / Ctrl+V of a single regular local file in legacy mode (queue disabled)
    {
        const QString srcB = files.filePath("legacy-single-src.txt");
        const QString dstB = b + "/legacy-single-src.txt";
        createFixture(srcB, "legacy-move-payload");
        auto *cutMimeB = new QMimeData;
        cutMimeB->setUrls({QUrl::fromLocalFile(srcB)});
        cutMimeB->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeB);

        const int expectedB = completed + 1;
        actions.pasteClipboardInto(rootB);
        waitJob(expectedB);
        verify(lastNativeMove && !QFile::exists(srcB) && readFile(dstB) == "legacy-move-payload",
               "Scenario B: single regular local file moved safely via legacy native path");
    }

    // Scenario C: Drag & Drop with Qt::MoveAction for a single regular file
    {
        const QString srcC = files.filePath("dnd-single-src.txt");
        const QString dstC = b + "/dnd-single-src.txt";
        createFixture(srcC, "dnd-move-payload");
        const int expectedC = completed + 1;
        actions.transfer({QUrl::fromLocalFile(srcC)}, rootB, Qt::MoveAction);
        waitJob(expectedC);
        verify(lastNativeMove && !QFile::exists(srcC) && readFile(dstC) == "dnd-move-payload",
               "Scenario C: single regular local file moved safely via Drag & Drop");
    }

    // Scenario D: Context menu Cut / Paste of a single regular file
    {
        const QString srcD = files.filePath("ctx-single-src.txt");
        const QString dstD = b + "/ctx-single-src.txt";
        createFixture(srcD, "ctx-move-payload");
        auto *cutMimeD = new QMimeData;
        cutMimeD->setUrls({QUrl::fromLocalFile(srcD)});
        cutMimeD->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeD);

        const int expectedD = completed + 1;
        actions.pasteClipboardInto(rootB);
        waitJob(expectedD);
        verify(lastNativeMove && !QFile::exists(srcD) && readFile(dstD) == "ctx-move-payload",
               "Scenario D: single regular local file moved safely via Cut / Paste menu path");
    }

    // Scenario E: Single symlink move refusal before modification
    {
        const QString targetE = files.filePath("symlink-target.txt");
        const QString linkE = files.filePath("symlink-src.lnk");
        createFixture(targetE, "target-payload");
        verify(::symlink(QFile::encodeName(targetE).constData(), QFile::encodeName(linkE).constData()) == 0,
               "Scenario E: symlink fixture created");
        auto *cutMimeE = new QMimeData;
        cutMimeE->setUrls({QUrl::fromLocalFile(linkE)});
        cutMimeE->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeE);

        lastFenceMessage.clear();
        const int beforeE = completed;
        actions.pasteClipboardInto(rootB);
        verify(completed == beforeE && QFileInfo(linkE).isSymLink() && !QFile::exists(b + "/symlink-src.lnk")
                   && lastFenceMessage == moveContainmentErrorMessage(),
               "Scenario E: single symlink move refused fail-closed before any modification");
    }

    // Scenario F: Unqualified directory move refusal (non-Btrfs / tmpfs) before modification (H.2A keeps non-Btrfs fail-closed)
    {
        const QString dirF = files.filePath("dir-f-src");
        verify(QDir().mkpath(dirF + "/sub"), "Scenario F: directory fixture created");
        createFixture(dirF + "/sub/item.txt", "data");
        auto *cutMimeF = new QMimeData;
        cutMimeF->setUrls({QUrl::fromLocalFile(dirF)});
        cutMimeF->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeF);

        simulateDirMoveUnsupportedFsMagic() = MSDOS_SUPER_MAGIC;
        lastFenceMessage.clear();
        const int beforeF = completed;
        actions.pasteClipboardInto(rootB);
        simulateDirMoveUnsupportedFsMagic() = 0;
        verify(completed == beforeF && QFileInfo::exists(dirF + "/sub/item.txt")
                   && !QFileInfo::exists(b + "/dir-f-src")
                   && !lastFenceMessage.isEmpty(),
               "Scenario F: non-Btrfs directory move refused fail-closed before any modification");
    }

    // Scenario G: Unqualified / non-Btrfs multiple regular files move refusal before modification
    {
        const QString fileG1 = files.filePath("multi-g1.txt");
        const QString fileG2 = files.filePath("multi-g2.txt");
        createFixture(fileG1, "g1");
        createFixture(fileG2, "g2");
        auto *cutMimeG = new QMimeData;
        cutMimeG->setUrls({QUrl::fromLocalFile(fileG1), QUrl::fromLocalFile(fileG2)});
        cutMimeG->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeG);

        simulateBatchMoveUnsupportedFsMagic() = MSDOS_SUPER_MAGIC;
        lastFenceMessage.clear();
        const int beforeG = completed;
        actions.pasteClipboardInto(rootB);
        simulateBatchMoveUnsupportedFsMagic() = 0;
        verify(completed == beforeG && QFile::exists(fileG1) && QFile::exists(fileG2)
                   && !QFile::exists(b + "/multi-g1.txt") && !QFile::exists(b + "/multi-g2.txt")
                   && !lastFenceMessage.isEmpty(),
               "Scenario G: non-Btrfs multiple files move refused fail-closed before any modification");
    }

    // Scenario H: Conflict during Move - no fallback to KIO
    {
        const QString srcH = files.filePath("conflict-h.txt");
        const QString dstH = b + "/conflict-h.txt";
        createFixture(srcH, "conflict-source");
        createFixture(dstH, "conflict-existing");
        auto *cutMimeH = new QMimeData;
        cutMimeH->setUrls({QUrl::fromLocalFile(srcH)});
        cutMimeH->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeH);

        QTimer::singleShot(0, &parent, [] {
            if (auto *dialog = qobject_cast<KIO::RenameDialog *>(QApplication::activeModalWidget())) {
                dialog->skipPressed();
            }
        });
        const int beforeH = completed;
        actions.pasteClipboardInto(rootB);
        verify(completed == beforeH && QFile::exists(srcH) && readFile(dstH) == "conflict-existing",
               "Scenario H: move conflict skipped without KIO fallback; both files preserved");
    }

    // Scenario I: Ordinary copy of files, directories and symlinks has no regression
    {
        const QString copySrc = files.filePath("copy-reg.txt");
        const QString copyDst = b + "/copy-reg.txt";
        createFixture(copySrc, "copy-reg-payload");
        const int expectedI = completed + 1;
        actions.transfer({QUrl::fromLocalFile(copySrc)}, rootB, Qt::CopyAction);
        waitJob(expectedI);
        verify(lastNativeCopy && QFile::exists(copySrc) && readFile(copyDst) == "copy-reg-payload",
               "Scenario I: ordinary copy of file continues to work natively without regression");
    }

    // Scenario J: Local move of single file on Btrfs
    {
        const QString srcJ = files.filePath("btrfs-single-src.txt");
        const QString dstJ = b + "/btrfs-single-src.txt";
        createFixture(srcJ, "btrfs-payload");
        const int expectedJ = completed + 1;
        actions.transfer({QUrl::fromLocalFile(srcJ)}, rootB, Qt::MoveAction);
        waitJob(expectedJ);
        verify(lastNativeMove && !QFile::exists(srcJ) && readFile(dstJ) == "btrfs-payload",
               "Scenario J: single regular file move executes cleanly on native filesystem");
    }

    // =========================================================================
    // Stage 5.3.2H.1.1 — Move Routing Containment Closure Tests (Groups A - D)
    // =========================================================================

    // Group A: transferUrls() / Duplicate Finder closure (zero KIO::move bypass for local)
    {
        // A1. Single regular local file Move via transferUrls() in legacy mode
        setTransferQueueProductionDispatchConnected(false);
        const QString srcA1 = files.filePath("tu-single-legacy.txt");
        const QString dstA1 = b + "/tu-single-legacy.txt";
        createFixture(srcA1, "transferUrls-legacy");

        int cbCountA1 = 0;
        bool cbNonNullA1 = false;
        int cbErrA1 = -1;
        bool cbDoneA1 = false;
        const int beforeUndoCount = completed;
        actions.transferUrls({QUrl::fromLocalFile(srcA1)}, rootB, Qt::MoveAction, [&](KJob *j) {
            ++cbCountA1;
            cbNonNullA1 = (j != nullptr);
            cbErrA1 = j ? j->error() : -1;
            cbDoneA1 = true;
        });
        verify(QTest::qWaitFor([&] { return cbDoneA1; }, 5000),
               "H.1.1-A1: legacy transferUrls single move finishes");
        verify(cbCountA1 == 1 && cbNonNullA1 && cbErrA1 == KJob::NoError,
               "H.1.1-A1: callback called once with valid native move job");
        verify(lastNativeMove && !QFile::exists(srcA1) && readFile(dstA1) == "transferUrls-legacy",
               "H.1.1-A1: single regular file moved natively via transferUrls (not KIO::move)");
        verify(completed == beforeUndoCount + 1, "H.1.1-A1: watchFileOperation observed completion");
        verify(undo.nativeUndoAvailable(), "H.1.1-A1: native move recorded in LocalMoveHistory (not KIO)");
        undo.undo();
        verify(QTest::qWaitFor([&] { return !undo.m_busy && QFile::exists(srcA1) && !QFile::exists(dstA1); }, 5000),
               "H.1.1-A1: Undo restores single file moved via transferUrls");
        QFile::remove(srcA1);

        // A2. Single regular local file Move via transferUrls() in production queue mode
        TransferQueueController queueCtlA(&parent);
        actions.setQueueController(&queueCtlA);
        setTransferQueueProductionDispatchConnected(true);
        const QString srcA2 = files.filePath("tu-single-queue.txt");
        const QString dstA2 = b + "/tu-single-queue.txt";
        createFixture(srcA2, "transferUrls-queue");

        int cbCountA2 = 0;
        bool cbNonNullA2 = false;
        int cbErrA2 = -1;
        actions.transferUrls({QUrl::fromLocalFile(srcA2)}, rootB, Qt::MoveAction, [&](KJob *j) {
            ++cbCountA2;
            cbNonNullA2 = (j != nullptr);
            cbErrA2 = j ? j->error() : -1;
        });
        verify(queueCtlA.snapshot().tasks.size() == 1
                   && queueCtlA.snapshot().tasks.front().backend == TransferBackendKind::LocalNative,
               "H.1.1-A2: queue transferUrls uses LocalNative backend");
        verify(QTest::qWaitFor([&] { return cbCountA2 == 1; }, 5000),
               "H.1.1-A2: queue transferUrls callback fires");
        verify(cbCountA2 == 1 && cbNonNullA2 && cbErrA2 == KJob::NoError,
               "H.1.1-A2: queue callback called once with LocalFileMoveJob result");
        verify(!QFile::exists(srcA2) && readFile(dstA2) == "transferUrls-queue",
               "H.1.1-A2: file moved cleanly via production queue");

        // A3. Multi-file, Directory, Symlink, Broken Symlink via transferUrls() -> fail-closed, NO KIO::move
        const QString m1 = files.filePath("tu-multi-1.txt");
        const QString m2 = files.filePath("tu-multi-2.txt");
        const QString dirS = files.filePath("tu-dir");
        const QString linkTarget = files.filePath("tu-target.txt");
        const QString linkValid = files.filePath("tu-link.lnk");
        const QString linkBroken = files.filePath("tu-broken.lnk");
        createFixture(m1, "m1");
        createFixture(m2, "m2");
        verify(QDir().mkpath(dirS + "/sub"), "H.1.1-A3: dir fixture created");
        createFixture(dirS + "/sub/inner.txt", "inner");
        createFixture(linkTarget, "t");
        verify(::symlink(QFile::encodeName(linkTarget).constData(), QFile::encodeName(linkValid).constData()) == 0
                   && ::symlink("/nonexistent/broken-target", QFile::encodeName(linkBroken).constData()) == 0,
               "H.1.1-A3: symlinks created");

        struct RefusalCase {
            const char *name;
            QList<QUrl> urls;
        };
        const QList<RefusalCase> tuRefusals = {
            {"multi-file", {QUrl::fromLocalFile(m1), QUrl::fromLocalFile(m2)}},
            {"directory", {QUrl::fromLocalFile(dirS)}},
            {"valid-symlink", {QUrl::fromLocalFile(linkValid)}},
            {"broken-symlink", {QUrl::fromLocalFile(linkBroken)}},
        };
        for (const bool useQueue : {false, true}) {
            setTransferQueueProductionDispatchConnected(useQueue);
            for (const auto &rc : tuRefusals) {
                const auto beforeTasks = queueCtlA.snapshot().tasks.size();
                const int beforeCompleted = completed;
                lastFenceMessage.clear();
                int cbCount = 0;
                KJob *cbJob = reinterpret_cast<KJob *>(0x1);
                actions.transferUrls(rc.urls, rootB, Qt::MoveAction, [&](KJob *j) {
                    ++cbCount;
                    cbJob = j;
                });
                verify(cbCount == 1 && cbJob == nullptr, "H.1.1-A3: transferUrls refused fail-closed without creating job");
                verify(queueCtlA.snapshot().tasks.size() == beforeTasks && completed == beforeCompleted,
                       "H.1.1-A3: zero queue tasks or KIO jobs created");
                verify(lastFenceMessage == moveContainmentErrorMessage(),
                       "H.1.1-A3: security fence message emitted");
            }
        }
        verify(QFile::exists(m1) && QFile::exists(m2) && QFile::exists(dirS + "/sub/inner.txt")
                   && QFileInfo(linkValid).isSymLink() && QFileInfo(linkBroken).isSymLink(),
               "H.1.1-A3: all refused sources completely preserved");

        // A4. Conflict with existing target via transferUrls() -> fails closed without overwrite or Undo
        const QString cSrc = files.filePath("tu-conflict.txt");
        const QString cDst = b + "/tu-conflict.txt";
        createFixture(cSrc, "new-source");
        createFixture(cDst, "existing-keep");
        for (const bool useQueue : {false, true}) {
            setTransferQueueProductionDispatchConnected(useQueue);
            int cbCount = 0;
            bool cbNonNull = false;
            int cbErr = 0;
            const auto beforeHistoryCount = undo.m_nativeHistory->count();
            actions.transferUrls({QUrl::fromLocalFile(cSrc)}, rootB, Qt::MoveAction, [&](KJob *j) {
                ++cbCount;
                cbNonNull = (j != nullptr);
                cbErr = j ? j->error() : 0;
            });
            verify(QTest::qWaitFor([&] { return cbCount == 1; }, 5000),
                   "H.1.1-A4: conflict transferUrls completes");
            verify(cbCount == 1 && cbNonNull && cbErr != KJob::NoError,
                   "H.1.1-A4: conflict fails cleanly without silent overwrite");
            verify(readFile(cSrc) == "new-source" && readFile(cDst) == "existing-keep",
                   "H.1.1-A4: source and existing destination intact");
            verify(undo.m_nativeHistory->count() == beforeHistoryCount,
                   "H.1.1-A4: failed conflict move never records history/undo");
        }

        // A5. Explicit overwrite attempt via LocalFileMoveJob remains fail-closed
        {
            LocalFileMoveJob ovJob(QUrl::fromLocalFile(cSrc), QUrl::fromLocalFile(cDst),
                                   nullptr, LocalFileMoveStrategy::Automatic, 16 * 1024, 0, {}, true,
                                   LocalFileIdentity::read(cDst));
            ovJob.setAutoDelete(false);
            QSignalSpy ovSpy(&ovJob, &KJob::result);
            ovJob.start();
            verify(ovSpy.wait(5000) && ovJob.error() != KJob::NoError && ovJob.moveError() == LocalFileMoveError::InvalidInput,
                   "H.1.1-A5: move overwrite strictly fail-closed");
            verify(readFile(cSrc) == "new-source" && readFile(cDst) == "existing-keep",
                   "H.1.1-A5: overwrite refusal leaves both files intact");
        }

        // A6. Cancellation of pending queued transferUrls task invokes callback once cleanly
        {
            setTransferQueueProductionDispatchConnected(true);
            queueCtlA.setDispatchPaused(true);
            const QString canSrc = files.filePath("tu-cancel.txt");
            const QString canDst = b + "/tu-cancel.txt";
            createFixture(canSrc, "cancel-payload");
            int cbCount = 0;
            KJob *cbJob = reinterpret_cast<KJob *>(0x1);
            actions.transferUrls({QUrl::fromLocalFile(canSrc)}, rootB, Qt::MoveAction, [&](KJob *j) {
                ++cbCount;
                cbJob = j;
            });
            const quint64 taskId = queueCtlA.snapshot().tasks.back().id;
            verify(queueCtlA.cancelTask(taskId), "H.1.1-A6: queued transferUrls task cancelled");
            verify(cbCount == 1 && cbJob == nullptr && QFile::exists(canSrc) && !QFile::exists(canDst),
                   "H.1.1-A6: cancelled transferUrls notifies once and preserves source");
            queueCtlA.setDispatchPaused(false);
        }

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // Group B: Central Move Classification for Local, Remote, Mixed & Invalid URLs (Queue + Legacy)
    {
        const QString locFile = files.filePath("mixed-local-src.txt");
        createFixture(locFile, "mixed-local-bytes");
        const QUrl locUrl = QUrl::fromLocalFile(locFile);
        const QUrl remoteSrc(QStringLiteral("sftp://198.51.100.1:1/share/remote-src.txt"));
        const QUrl remoteDst(QStringLiteral("sftp://198.51.100.1:1/share/remote-dst"));

        // B1. Policy classification checks
        verify(MoveSafetyPolicy::evaluate({locUrl}, rootB, true).decision == MovePolicyDecision::AllowedSingleRegularLocalFile,
               "H.1.1-B1: local regular -> local dir allowed");
        verify(MoveSafetyPolicy::evaluate({locUrl, remoteSrc}, rootB, true).decision == MovePolicyDecision::RefusedMixedSources,
               "H.1.1-B1: local + remote -> local refused");
        verify(MoveSafetyPolicy::evaluate({locUrl, remoteSrc}, remoteDst, true).decision == MovePolicyDecision::RefusedMixedSources,
               "H.1.1-B1: local + remote -> remote refused");
        verify(MoveSafetyPolicy::evaluate({locUrl}, remoteDst, true).decision == MovePolicyDecision::RefusedLocalToRemote,
               "H.1.1-B1: local -> remote refused");
        verify(MoveSafetyPolicy::evaluate({remoteSrc}, rootB, true).decision == MovePolicyDecision::RefusedRemoteToLocal,
               "H.1.1-B1: remote -> local refused");
        verify(MoveSafetyPolicy::evaluate({remoteSrc}, remoteDst, true).decision == MovePolicyDecision::AllowedRemoteToRemote,
               "H.1.1-B1: remote -> remote remains allowed outside local engine");
        verify(MoveSafetyPolicy::evaluate({QUrl()}, rootB, true).decision == MovePolicyDecision::RefusedInvalidInput,
               "H.1.1-B1: invalid source URL refused");
        verify(MoveSafetyPolicy::evaluate({locUrl}, QUrl(QStringLiteral("thispc:/")), true).decision == MovePolicyDecision::RefusedInvalidInput,
               "H.1.1-B1: virtual destination URL refused");
        verify(isLocalMoveContainmentBlocked({locUrl, remoteSrc}, rootB, true),
               "H.1.1-B1: isLocalMoveContainmentBlocked catches mixed sources -> local");
        verify(isLocalMoveContainmentBlocked({locUrl, remoteSrc}, remoteDst, true),
               "H.1.1-B1: isLocalMoveContainmentBlocked catches mixed sources -> remote");
        verify(isLocalMoveContainmentBlocked({locUrl}, remoteDst, true),
               "H.1.1-B1: isLocalMoveContainmentBlocked catches local -> remote");

        // B2. Runtime containment in Queue & Legacy modes: zero jobs, zero remote connection attempts
        TransferQueueController queueCtlB(&parent);
        actions.setQueueController(&queueCtlB);
        struct DeniedRouteCase {
            const char *name;
            QList<QUrl> srcs;
            QUrl dst;
        };
        const QList<DeniedRouteCase> deniedCases = {
            {"mixed->local", {locUrl, remoteSrc}, rootB},
            {"mixed->remote", {locUrl, remoteSrc}, remoteDst},
            {"local->remote", {locUrl}, remoteDst},
            {"remote->local", {remoteSrc}, rootB},
            {"invalid-src", {QUrl(QStringLiteral("thispcsearch:/?q=test"))}, rootB},
            {"invalid-dst", {locUrl}, QUrl(QStringLiteral("thispc:/"))},
        };
        for (const bool useQueue : {false, true}) {
            setTransferQueueProductionDispatchConnected(useQueue);
            for (const auto &dc : deniedCases) {
                const int beforeComp = completed;
                const auto beforeTasks = queueCtlB.snapshot().tasks.size();
                lastFenceMessage.clear();

                // transfer (DnD)
                actions.transfer(dc.srcs, dc.dst, Qt::MoveAction);
                verify(completed == beforeComp && queueCtlB.snapshot().tasks.size() == beforeTasks && !lastFenceMessage.isEmpty(),
                       "H.1.1-B2: disallowed DnD move created zero jobs and emitted fence");

                // pasteClipboardInto (Cut+Paste)
                lastFenceMessage.clear();
                auto *mime = new QMimeData;
                mime->setUrls(dc.srcs);
                mime->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
                app.clipboard()->setMimeData(mime);
                actions.pasteClipboardInto(dc.dst);
                verify(completed == beforeComp && queueCtlB.snapshot().tasks.size() == beforeTasks && !lastFenceMessage.isEmpty(),
                       "H.1.1-B2: disallowed Cut+Paste move created zero jobs and emitted fence");

                // transferUrls
                int cbCount = 0;
                KJob *cbJob = reinterpret_cast<KJob *>(0x1);
                actions.transferUrls(dc.srcs, dc.dst, Qt::MoveAction, [&](KJob *j) {
                    ++cbCount;
                    cbJob = j;
                });
                verify(cbCount == 1 && cbJob == nullptr && queueCtlB.snapshot().tasks.size() == beforeTasks,
                       "H.1.1-B2: disallowed transferUrls created zero jobs");
            }
        }
        verify(readFile(locFile) == "mixed-local-bytes",
               "H.1.1-B2: local source untouched across all refused mixed/local-remote routes");

        // B3. Remote-only -> Remote-only reaches remote dispatch (tested via runner seam, zero DNS hang)
        setTransferQueueProductionDispatchConnected(false);
        interceptFileJobs = true;
        dispatch = {};
        actions.transfer({remoteSrc}, remoteDst, Qt::MoveAction);
        verify(dispatch.kind == QStringLiteral("move") && dispatch.sources == QList<QUrl>{remoteSrc} && dispatch.destination == remoteDst,
               "H.1.1-B3: remote-only move continues to reach remote KIO path");
        interceptFileJobs = false;
        dispatch = {};

        // B4. Copy for mixed and local->local multi remains functional (no regression)
        const QString cpy1 = files.filePath("copy-keep-1.txt");
        const QString cpy2 = files.filePath("copy-keep-2.txt");
        createFixture(cpy1, "c1");
        createFixture(cpy2, "c2");
        const int expectedCopyMulti = completed + 1;
        actions.transfer({QUrl::fromLocalFile(cpy1), QUrl::fromLocalFile(cpy2)}, rootB, Qt::CopyAction);
        waitJob(expectedCopyMulti);
        verify(readFile(b + "/copy-keep-1.txt") == "c1" && readFile(b + "/copy-keep-2.txt") == "c2"
                   && QFile::exists(cpy1) && QFile::exists(cpy2),
               "H.1.1-B4: multi-file local copy remains fully functional");

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // Group C: Object Type Classification via lstat / S_ISREG (FIFO, Socket, Devices, Symlinks, Dir, Missing)
    {
        const QString regC = files.filePath("obj-regular.txt");
        const QString dirC = files.filePath("obj-dir");
        const QString lnkFileC = files.filePath("obj-link-file.lnk");
        const QString lnkDirC = files.filePath("obj-link-dir.lnk");
        const QString lnkBrokenC = files.filePath("obj-link-broken.lnk");
        const QString fifoC = files.filePath("obj-pipe.fifo");
        const QString sockC = files.filePath("obj-unix.sock");
        const QString devSimC = files.filePath("obj-dev-sim");
        const QString missingC = files.filePath("obj-does-not-exist");

        createFixture(regC, "reg");
        verify(QDir().mkdir(dirC), "H.1.1-C: dir created");
        verify(::symlink(QFile::encodeName(regC).constData(), QFile::encodeName(lnkFileC).constData()) == 0
                   && ::symlink(QFile::encodeName(dirC).constData(), QFile::encodeName(lnkDirC).constData()) == 0
                   && ::symlink("/no/such/file", QFile::encodeName(lnkBrokenC).constData()) == 0,
               "H.1.1-C: symlinks created");
        verify(::mkfifo(QFile::encodeName(fifoC).constData(), 0600) == 0, "H.1.1-C: FIFO created");
        verify(::mknod(QFile::encodeName(sockC).constData(), S_IFSOCK | 0600, 0) == 0, "H.1.1-C: Unix socket created");

        verify(isSingleRegularLocalFile({QUrl::fromLocalFile(regC)}), "H.1.1-C: regular file allowed");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(dirC)}), "H.1.1-C: directory blocked");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(lnkFileC)}), "H.1.1-C: symlink to file blocked");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(lnkDirC)}), "H.1.1-C: symlink to dir blocked");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(lnkBrokenC)}), "H.1.1-C: broken symlink blocked");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(fifoC)}), "H.1.1-C: FIFO blocked without opening");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(sockC)}), "H.1.1-C: Unix socket blocked");
        verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(missingC)}), "H.1.1-C: missing file blocked");

        // Simulated char and block devices via lstat hook (no root/mknod dev needed)
        for (const mode_t devMode : {static_cast<mode_t>(S_IFCHR | 0600), static_cast<mode_t>(S_IFBLK | 0600)}) {
            simulateLstatHook() = [devSimC, devMode](const QString &p, struct stat *st) {
                if (p == devSimC) {
                    *st = {};
                    st->st_mode = devMode;
                    return true;
                }
                return false;
            };
            verify(!isSingleRegularLocalFile({QUrl::fromLocalFile(devSimC)}),
                   "H.1.1-C: simulated device blocked by isSingleRegularLocalFile");
            verify(!LocalFileIdentity::read(devSimC).valid,
                   "H.1.1-C: simulated device rejected by LocalFileIdentity::read");
            TransferQueueController queueCtlDev(&parent);
            actions.setQueueController(&queueCtlDev);
            setTransferQueueProductionDispatchConnected(true);
            actions.transfer({QUrl::fromLocalFile(devSimC)}, rootB, Qt::MoveAction);
            verify(queueCtlDev.snapshot().tasks.isEmpty(),
                   "H.1.1-C: simulated device blocked before queue job creation");
            setTransferQueueProductionDispatchConnected(false);
            actions.setQueueController(nullptr);
            simulateLstatHook() = {};
        }

        // Blocking FIFO/socket runtime moves in both Queue and Legacy modes
        TransferQueueController queueCtlObj(&parent);
        actions.setQueueController(&queueCtlObj);
        for (const bool useQueue : {false, true}) {
            setTransferQueueProductionDispatchConnected(useQueue);
            for (const QString &specialPath : {fifoC, sockC}) {
                const int beforeComp = completed;
                lastFenceMessage.clear();
                actions.transfer({QUrl::fromLocalFile(specialPath)}, rootB, Qt::MoveAction);
                verify(completed == beforeComp && queueCtlObj.snapshot().tasks.isEmpty()
                           && lastFenceMessage == moveContainmentErrorMessage(),
                       "H.1.1-C: special file DnD move blocked cleanly before job creation");
                int cbCount = 0;
                KJob *cbJob = reinterpret_cast<KJob *>(0x1);
                actions.transferUrls({QUrl::fromLocalFile(specialPath)}, rootB, Qt::MoveAction, [&](KJob *j) {
                    ++cbCount;
                    cbJob = j;
                });
                verify(cbCount == 1 && cbJob == nullptr && queueCtlObj.snapshot().tasks.isEmpty(),
                       "H.1.1-C: special file transferUrls move blocked cleanly");
            }
        }
        setTransferQueueProductionDispatchConnected(false);
        actions.setQueueController(nullptr);
        verify(QFileInfo::exists(fifoC) && QFileInfo::exists(sockC),
               "H.1.1-C: FIFO and socket fixtures intact");
    }

    // Group D: Defense-in-depth checks (LocalTransferJob(move=true) & KIO UndoController::recordCopyJob)
    {
        const QString dSrc = files.filePath("defense-src.txt");
        createFixture(dSrc, "defense");
        auto *ltj = new LocalTransferJob({QUrl::fromLocalFile(dSrc)}, rootB, true, &parent);
        QSignalSpy ltjSpy(ltj, &KJob::result);
        ltj->start();
        verify(ltjSpy.wait(5000) && ltj->error() != KJob::NoError && ltj->errorText() == moveContainmentErrorMessage(),
               "H.1.1-D: LocalTransferJob(move=true) remains strictly fail-closed");
        verify(readFile(dSrc) == "defense", "H.1.1-D: source untouched");

        // Calling startNativeSingleFileTransfer directly with unqualified multiple files must NOT dispatch LocalTransferJob(move=true)
        const int beforeCompD = completed;
        lastFenceMessage.clear();
        simulateBatchMoveUnsupportedFsMagic() = MSDOS_SUPER_MAGIC;
        const bool handledMulti = actions.startNativeSingleFileTransfer(
            {QUrl::fromLocalFile(dSrc), QUrl::fromLocalFile(files.filePath("btrfs-single-src.txt"))},
            rootB, true, false, {}, {});
        simulateBatchMoveUnsupportedFsMagic() = 0;
        verify(handledMulti && completed == beforeCompD && !lastFenceMessage.isEmpty(),
               "H.1.1-D: direct startNativeSingleFileTransfer non-Btrfs multi-move blocked fail-closed");

        // Defense in depth: UndoController::recordCopyJob must never record uncontained local cross-dir KIO::move
        auto *unsafeKioMove = KIO::move({QUrl::fromLocalFile(dSrc)}, rootB, KIO::HideProgressInfo);
        unsafeKioMove->setUiDelegate(nullptr);
        const quint64 beforeIrrev = undo.m_irreversibleSerial;
        const quint64 beforeKioSerial = undo.m_manager->currentCommandSerialNumber();
        QSignalSpy kioSpy(unsafeKioMove, &KJob::result);
        undo.recordCopyJob(unsafeKioMove);
        unsafeKioMove->kill(KJob::EmitResult);
        verify(kioSpy.count() > 0 || kioSpy.wait(2000),
               "H.1.1-D: killed KIO::move emits result");
        verify(undo.m_manager->currentCommandSerialNumber() == beforeKioSerial
                   && !undo.kioUndoAvailable() && undo.m_irreversibleSerial > beforeIrrev,
               "H.1.1-D: cross-directory local KIO::move never recorded in KIO UndoManager");
    }

    // ------------------------------------------------------------------
    // Stage 5.3.2H.2A: Same-Volume Btrfs Directory Move Integration (Group E)
    // ------------------------------------------------------------------
    {
        struct statfs rootSfs {};
        verify(::statfs(QFile::encodeName(a).constData(), &rootSfs) == 0
                   && static_cast<unsigned long>(rootSfs.f_type) == BTRFS_SUPER_MAGIC,
               "H.2A-E: test root resides on real Btrfs");

        // E1: Ctrl+X / Ctrl+V (pasteClipboardInto) in Legacy mode + Undo + Redo
        const QString legDir = a + QStringLiteral("/h2a-legacy-dir");
        const QString legDst = b + QStringLiteral("/h2a-legacy-dir");
        verify(QDir().mkpath(legDir + QStringLiteral("/nested")), "H.2A-E1: dir tree created");
        createFixture(legDir + QStringLiteral("/nested/file.txt"), "legacy-dir-data");

        auto *cutMime = new QMimeData;
        cutMime->setUrls({QUrl::fromLocalFile(legDir)});
        cutMime->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        QApplication::clipboard()->setMimeData(cutMime);

        setTransferQueueProductionDispatchConnected(false);
        actions.setQueueController(nullptr);
        const int beforeLegacyComp = completed;
        actions.pasteClipboardInto(rootB);
        verify(QTest::qWaitFor([&] { return completed == beforeLegacyComp + 1; }, 10000)
                   && lastError == 0
                   && lastNativeDirMove
                   && QFileInfo::exists(legDst + QStringLiteral("/nested/file.txt"))
                   && !QFileInfo::exists(legDir),
               "H.2A-E1: pasteClipboardInto moved qualified Btrfs dir cleanly via LocalDirectoryMoveJob");

        // Undo + Redo via UndoController
        verify(undo.nativeUndoAvailable(), "H.2A-E1: Undo available for directory move");
        undo.undo();
        verify(QTest::qWaitFor([&] {
                   return !undo.m_busy && QFileInfo::exists(legDir + QStringLiteral("/nested/file.txt"))
                       && !QFileInfo::exists(legDst);
               }, 10000) && undo.nativeRedoAvailable(),
               "H.2A-E1: Undo moved directory back cleanly");
        undo.redo();
        verify(QTest::qWaitFor([&] {
                   return !undo.m_busy && QFileInfo::exists(legDst + QStringLiteral("/nested/file.txt"))
                       && !QFileInfo::exists(legDir);
               }, 10000),
               "H.2A-E1: Redo re-moved directory cleanly");

        // E2: Drag & Drop (transfer Qt::MoveAction) in Queue mode
        const QString qDir = a + QString::fromUtf8("/h2a-kolejka-zażółć");
        const QString qDst = b + QString::fromUtf8("/h2a-kolejka-zażółć");
        verify(QDir().mkpath(qDir + QStringLiteral("/sub")), "H.2A-E2: queue dir tree created");
        createFixture(qDir + QStringLiteral("/sub/item.dat"), "queue-payload-xyz");

        TransferQueueController queueCtlE(&parent);
        actions.setQueueController(&queueCtlE);
        setTransferQueueProductionDispatchConnected(true);
        const int beforeCompQ = completed;
        actions.transfer({QUrl::fromLocalFile(qDir)}, rootB, Qt::MoveAction);
        const auto qSnap1 = queueCtlE.snapshot();
        verify(qSnap1.tasks.size() == 1 && qSnap1.tasks.first().backend == TransferBackendKind::LocalNative,
               "H.2A-E2: queued directory move uses LocalNative backend");
        verify(QTest::qWaitFor([&] {
                   const auto s = queueCtlE.snapshot();
                   return !s.tasks.isEmpty() && s.tasks.first().state == TransferTaskState::Completed;
               }, 10000),
               "H.2A-E2: queued directory move reaches Completed state");
        verify(completed == beforeCompQ && !QFileInfo::exists(qDir)
                   && fixtureData(qDst + QStringLiteral("/sub/item.dat")) == "queue-payload-xyz",
               "H.2A-E2: zero legacy OperationManager double-tracking and intact payload");

        // E3: Duplicate Finder transferUrls() STILL blocks directory Move fail-closed!
        lastFenceMessage.clear();
        int dfCbCount = 0;
        KJob *dfCbJob = reinterpret_cast<KJob *>(0x1);
        actions.transferUrls({QUrl::fromLocalFile(qDst)}, rootA, Qt::MoveAction, [&](KJob *j) {
            ++dfCbCount;
            dfCbJob = j;
        });
        verify(dfCbCount == 1 && dfCbJob == nullptr && queueCtlE.snapshot().tasks.size() == 1
                   && QFileInfo::exists(qDst + QStringLiteral("/sub/item.dat"))
                   && lastFenceMessage == moveContainmentErrorMessage(),
               "H.2A-E3: Duplicate Finder transferUrls still refuses directory Move fail-closed");

        // E4: Directory collision in Queue mode fails closed without overwrite dialog & without overwrite
        const QString colSrc = a + QStringLiteral("/h2a-col");
        const QString colDst = b + QStringLiteral("/h2a-col");
        verify(QDir().mkdir(colSrc) && QDir().mkdir(colDst), "H.2A-E4: collision dirs created");
        createFixture(colSrc + QStringLiteral("/src.txt"), "SRC");
        createFixture(colDst + QStringLiteral("/dst.txt"), "DST");
        actions.transfer({QUrl::fromLocalFile(colSrc)}, rootB, Qt::MoveAction);
        verify(QTest::qWaitFor([&] {
                   const auto s = queueCtlE.snapshot();
                   return s.tasks.size() == 2 && s.tasks.at(1).state == TransferTaskState::Failed;
               }, 10000),
               "H.2A-E4: existing destination dir fails closed in queue without overwrite dialog");
        verify(QFileInfo::exists(colSrc + QStringLiteral("/src.txt"))
                   && QFileInfo::exists(colDst + QStringLiteral("/dst.txt")),
               "H.2A-E4: both colliding directories preserved completely");

        // E5: Descendant cycle & batch multi-dir & mixed file+dir blocked before queue enqueue
        const QString descParent = a + QStringLiteral("/h2a-cyc");
        const QString descChild = descParent + QStringLiteral("/sub");
        verify(QDir().mkpath(descChild), "H.2A-E5: cycle dir created");
        actions.transfer({QUrl::fromLocalFile(descParent)}, QUrl::fromLocalFile(descChild), Qt::MoveAction);
        actions.transfer({QUrl::fromLocalFile(descParent), QUrl::fromLocalFile(colSrc)}, rootB, Qt::MoveAction);
        verify(queueCtlE.snapshot().tasks.size() == 2,
               "H.2A-E5: cycle and multi-directory moves blocked before queue task creation");

        // E6: Watched moved directory notification safety (DirectoryWatchController reentrancy compatibility)
        const QString watchSrc = a + QStringLiteral("/h2a-watched-dir");
        const QString watchDst = b + QStringLiteral("/h2a-watched-dir");
        verify(QDir().mkdir(watchSrc), "H.2A-E6: watched dir created");
        createFixture(watchSrc + QStringLiteral("/w.txt"), "W");
        DirectoryWatchController watcher;
        int dummyPrimary = 1;
        int dummySplit = 2;
        watcher.setWatchedUrl(&dummyPrimary, QUrl::fromLocalFile(a));
        watcher.setWatchedUrl(&dummySplit, QUrl::fromLocalFile(watchSrc));
        int deletedSignals = 0;
        QObject::connect(&watcher, &DirectoryWatchController::directoryDeleted, &watcher, [&](const QUrl &u) {
            if (sameLocation(u, QUrl::fromLocalFile(watchSrc))) {
                ++deletedSignals;
                watcher.removeConsumer(&dummySplit);
                watcher.setWatchedUrl(&dummySplit, QUrl::fromLocalFile(watchDst));
            }
        });
        actions.transfer({QUrl::fromLocalFile(watchSrc)}, rootB, Qt::MoveAction);
        verify(QTest::qWaitFor([&] {
                   const auto s = queueCtlE.snapshot();
                   return s.tasks.size() == 3 && s.tasks.at(2).state == TransferTaskState::Completed;
               }, 10000),
               "H.2A-E6: watched dir moved safely with DirectoryWatchController without crash");
        QTest::qWait(50);
        watcher.clear();

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // ------------------------------------------------------------------
    // Stage 5.3.2H.2B.1: Same-Volume Btrfs Batch File Move Integration (Group F)
    // ------------------------------------------------------------------
    {
        // F1: Legacy Cut+Paste (Ctrl+X / Ctrl+V) of 3 regular files across 2 Btrfs folders + Undo + Redo
        const QString fDir1 = a + QStringLiteral("/h2b1-src1");
        const QString fDir2 = a + QStringLiteral("/h2b1-src2");
        verify(QDir().mkpath(fDir1) && QDir().mkpath(fDir2), "H.2B.1-F1: dirs created");
        const QString bf1 = fDir1 + QString::fromUtf8("/zażółć.txt");
        const QString bf2 = fDir2 + QStringLiteral("/drugi plik.txt");
        const QString bf3 = fDir1 + QStringLiteral("/.ukryty");
        createFixture(bf1, "B1");
        createFixture(bf2, "B2");
        createFixture(bf3, "B3");

        auto *cutBatch = new QMimeData;
        cutBatch->setUrls({QUrl::fromLocalFile(bf1), QUrl::fromLocalFile(bf2), QUrl::fromLocalFile(bf3)});
        cutBatch->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        QApplication::clipboard()->setMimeData(cutBatch);

        setTransferQueueProductionDispatchConnected(false);
        actions.setQueueController(nullptr);
        const int beforeLegacyB = completed;
        actions.pasteClipboardInto(rootB);
        const QString bt1 = b + QString::fromUtf8("/zażółć.txt");
        const QString bt2 = b + QStringLiteral("/drugi plik.txt");
        const QString bt3 = b + QStringLiteral("/.ukryty");
        verify(QTest::qWaitFor([&] { return completed == beforeLegacyB + 1; }, 10000)
                   && lastError == 0
                   && !QFileInfo::exists(bf1) && !QFileInfo::exists(bf2) && !QFileInfo::exists(bf3)
                   && fixtureData(bt1) == "B1" && fixtureData(bt2) == "B2" && fixtureData(bt3) == "B3",
               "H.2B.1-F1: legacy Cut+Paste moved 3 Btrfs files atomically");

        verify(undo.nativeUndoAvailable(), "H.2B.1-F1: compound batch Undo available");
        undo.undo();
        verify(QTest::qWaitFor([&] {
                   return !undo.m_busy && fixtureData(bf1) == "B1"
                       && fixtureData(bf2) == "B2" && fixtureData(bf3) == "B3"
                       && !QFileInfo::exists(bt1);
               }, 10000) && undo.nativeRedoAvailable(),
               "H.2B.1-F1: Undo restored all 3 files across 2 folders");
        undo.redo();
        verify(QTest::qWaitFor([&] {
                   return !undo.m_busy && fixtureData(bt1) == "B1"
                       && fixtureData(bt2) == "B2" && fixtureData(bt3) == "B3"
                       && !QFileInfo::exists(bf1);
               }, 10000),
               "H.2B.1-F1: Redo re-applied batch move cleanly");

        // F2: Queue mode Drag & Drop -> single LocalNative task, plus Duplicate Finder STILL blocks multi-file!
        TransferQueueController queueCtlF(&parent);
        actions.setQueueController(&queueCtlF);
        setTransferQueueProductionDispatchConnected(true);
        const QString qf1 = fDir1 + QStringLiteral("/q1.txt");
        const QString qf2 = fDir1 + QStringLiteral("/q2.txt");
        createFixture(qf1, "Q1");
        createFixture(qf2, "Q2");
        const int beforeCompF = completed;
        actions.transfer({QUrl::fromLocalFile(qf1), QUrl::fromLocalFile(qf2)}, rootB, Qt::MoveAction);
        verify(queueCtlF.snapshot().tasks.size() == 1
                   && queueCtlF.snapshot().tasks.first().backend == TransferBackendKind::LocalNative,
               "H.2B.1-F2: queued batch move creates single LocalNative task");
        verify(QTest::qWaitFor([&] {
                   const auto s = queueCtlF.snapshot();
                   return !s.tasks.isEmpty() && s.tasks.first().state == TransferTaskState::Completed;
               }, 10000),
               "H.2B.1-F2: queued batch move completes");
        verify(completed == beforeCompF
                   && fixtureData(b + QStringLiteral("/q1.txt")) == "Q1"
                   && fixtureData(b + QStringLiteral("/q2.txt")) == "Q2",
               "H.2B.1-F2: queued batch move preserved data with zero legacy double-tracking");

        // Duplicate Finder MUST still reject multi-file Move even on Btrfs
        lastFenceMessage.clear();
        int dupCbCount = 0;
        KJob *dupCbJob = reinterpret_cast<KJob *>(0x1);
        actions.transferUrls(
            {QUrl::fromLocalFile(b + QStringLiteral("/q1.txt")), QUrl::fromLocalFile(b + QStringLiteral("/q2.txt"))},
            rootA, Qt::MoveAction, [&](KJob *j) {
                ++dupCbCount;
                dupCbJob = j;
            });
        verify(dupCbCount == 1 && dupCbJob == nullptr
                   && queueCtlF.snapshot().tasks.size() == 1
                   && lastFenceMessage == moveContainmentErrorMessage(),
               "H.2B.1-F3: Duplicate Finder transferUrls still blocks Btrfs multi-file Move fail-closed");

        // F4: Mixed file + directory batch succeeds in production FileActions::transfer + compound Undo/Redo
        const QString mixDir = fDir1 + QStringLiteral("/mixdir");
        verify(QDir().mkdir(mixDir), "H.2B.2B-F4: dir created");
        const QString mixFile = fDir1 + QStringLiteral("/mixfile.txt");
        createFixture(mixFile, "MIX");
        actions.transfer({QUrl::fromLocalFile(mixFile), QUrl::fromLocalFile(mixDir)}, rootB, Qt::MoveAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = queueCtlF.snapshot();
            return snap.tasks.size() == 2
                && snap.tasks.at(1).state == TransferTaskState::Completed;
        }, 10000), "H.2B.2B-F4: mixed file+dir batch completes via production queue");
        verify(!QFileInfo::exists(mixFile) && !QFileInfo::exists(mixDir)
                   && QFileInfo::exists(b + QStringLiteral("/mixfile.txt"))
                   && QFileInfo::exists(b + QStringLiteral("/mixdir")),
               "H.2B.2B-F4: mixed file+dir batch moved to destination");
        verify(undo.nativeUndoAvailable(), "H.2B.2B-F4: compound Undo available after queued mixed Move");
        undo.undo();
        verify(QTest::qWaitFor([&] {
            return !undo.m_busy && QFileInfo::exists(mixFile) && QFileInfo::exists(mixDir)
                && !QFileInfo::exists(b + QStringLiteral("/mixfile.txt"))
                && !QFileInfo::exists(b + QStringLiteral("/mixdir"));
        }, 10000) && undo.nativeRedoAvailable(), "H.2B.2B-F4: UndoController restored mixed batch");
        undo.redo();
        verify(QTest::qWaitFor([&] {
            return !undo.m_busy && !QFileInfo::exists(mixFile) && !QFileInfo::exists(mixDir)
                && QFileInfo::exists(b + QStringLiteral("/mixfile.txt"))
                && QFileInfo::exists(b + QStringLiteral("/mixdir"));
        }, 10000), "H.2B.2B-F4: UndoController Redo re-moved mixed batch");

        // F5: 2-directory batch move succeeds in production FileActions::transfer, while transferUrls stays blocked
        const QString mixDir2 = fDir1 + QStringLiteral("/mixdir2");
        const QString mixDir3 = fDir1 + QStringLiteral("/mixdir3");
        verify(QDir().mkdir(mixDir2) && QDir().mkdir(mixDir3), "H.2B.2B-F5: dirs created");
        actions.transfer({QUrl::fromLocalFile(mixDir2), QUrl::fromLocalFile(mixDir3)}, rootB, Qt::MoveAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = queueCtlF.snapshot();
            return snap.tasks.size() == 3
                && snap.tasks.at(2).state == TransferTaskState::Completed;
        }, 10000), "H.2B.2B-F5: 2-directory batch completes via production FileActions::transfer");
        verify(!QFileInfo::exists(mixDir2) && !QFileInfo::exists(mixDir3)
                   && QFileInfo::exists(b + QStringLiteral("/mixdir2"))
                   && QFileInfo::exists(b + QStringLiteral("/mixdir3")),
               "H.2B.2B-F5: 2-directory batch moved to destination");

        lastFenceMessage.clear();
        int dupDirCb = 0;
        KJob *dupDirJob = reinterpret_cast<KJob *>(0x1);
        actions.transferUrls({QUrl::fromLocalFile(b + QStringLiteral("/mixdir2")),
                              QUrl::fromLocalFile(b + QStringLiteral("/mixdir3"))},
                             QUrl::fromLocalFile(fDir1), Qt::MoveAction,
                             [&](KJob *j) {
                                 ++dupDirCb;
                                 dupDirJob = j;
                             });
        verify(dupDirCb == 1 && dupDirJob == nullptr
                   && queueCtlF.snapshot().tasks.size() == 3
                   && !lastFenceMessage.isEmpty(),
               "H.2B.2A-F6: Duplicate Finder transferUrls still blocks 2-directory batch move");

        // H.2C.1A-F7: Production GUI dispatchers stay locked against ext4 directory & batch moves
        {
            const QString gDir1 = fDir1 + QStringLiteral("/gui_ext4_d1");
            const QString gDir2 = fDir1 + QStringLiteral("/gui_ext4_d2");
            const QString gFile1 = fDir1 + QStringLiteral("/gui_ext4_f1.txt");
            const QString gFile2 = fDir1 + QStringLiteral("/gui_ext4_f2.txt");
            verify(QDir().mkdir(gDir1) && QDir().mkdir(gDir2), "H.2C.1A-F7: dirs");
            createFixture(gFile1, "G1");
            createFixture(gFile2, "G2");

            simulateDirMoveExt4Filesystem() = true;
            simulateBatchMoveExt4Filesystem() = true;
            simulateBatchMoveUnsupportedFsMagic() = EXT4_SUPER_MAGIC;

            const auto evalSingleDir = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(gDir1)}, rootB, true);
            const auto evalFileBatch = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(gFile1), QUrl::fromLocalFile(gFile2)}, rootB, true);
            const auto evalMixedBatch = MoveSafetyPolicy::evaluate(
                {QUrl::fromLocalFile(gFile1), QUrl::fromLocalFile(gDir1)}, rootB, true);
            verify(!evalSingleDir.allowed
                       && !evalFileBatch.allowed
                       && !evalMixedBatch.allowed
                       && evalSingleDir.decision == MovePolicyDecision::RefusedLocalContainment
                       && evalFileBatch.decision == MovePolicyDecision::RefusedLocalContainment
                       && evalMixedBatch.decision == MovePolicyDecision::RefusedLocalContainment,
                   "H.2C.1A-F7a: MoveSafetyPolicy::evaluate locks ext4 dir/batch moves out of GUI");

            lastFenceMessage.clear();
            actions.transfer({QUrl::fromLocalFile(gDir1)}, rootB, Qt::MoveAction);
            verify(queueCtlF.snapshot().tasks.size() == 3
                       && !lastFenceMessage.isEmpty()
                       && QFileInfo::exists(gDir1)
                       && !QFileInfo::exists(b + QStringLiteral("/gui_ext4_d1")),
                   "H.2C.1A-F7b: FileActions::transfer refuses single dir Move on simulated ext4 without fallback");

            lastFenceMessage.clear();
            actions.transfer({QUrl::fromLocalFile(gFile1), QUrl::fromLocalFile(gDir2)}, rootB, Qt::MoveAction);
            verify(queueCtlF.snapshot().tasks.size() == 3
                       && !lastFenceMessage.isEmpty()
                       && QFileInfo::exists(gFile1) && QFileInfo::exists(gDir2)
                       && !QFileInfo::exists(b + QStringLiteral("/gui_ext4_f1.txt"))
                       && !QFileInfo::exists(b + QStringLiteral("/gui_ext4_d2")),
                   "H.2C.1A-F7c: FileActions::transfer refuses mixed batch Move on simulated ext4 without fallback");

            simulateDirMoveExt4Filesystem() = false;
            simulateBatchMoveExt4Filesystem() = false;
            simulateBatchMoveUnsupportedFsMagic() = 0;
        }

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // LIFE-01-FIX-04: UndoController::startNativeHistoryJob() must schedule deleteLater()
    // on finished setAutoDelete(false) KJobs across Single-File, Single-Dir, Batch, and Mixed Batch.
    {
        auto drainDeferredDeletes = [&] {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        auto waitUndoReady = [&] {
            verify(QTest::qWaitFor([&] {
                return undoAction.isEnabled() || redoAction.isEnabled();
            }, 10000), "LIFE-01-FIX-04: undo/redo operation finished");
            drainDeferredDeletes();
        };

        // 1. Move without Undo -> KJob cleaned up
        const QString lifeFile = a + QStringLiteral("/life_fix_file.txt");
        const QString lifeFileDst = b + QStringLiteral("/life_fix_file.txt");
        createFixture(lifeFile, "LIFE_FILE");
        const int baseCompleted = completed;
        actions.transfer({QUrl::fromLocalFile(lifeFile)}, rootB, Qt::MoveAction);
        waitJob(baseCompleted + 1);
        drainDeferredDeletes();
        verify(!QFile::exists(lifeFile) && fixtureData(lifeFileDst) == "LIFE_FILE"
                   && parent.findChildren<KJob *>().isEmpty(),
               "LIFE-01-FIX-04-1: Move without Undo leaves 0 live KJobs");

        // 2 & 3. Move -> Undo -> Redo -> 0 live KJobs
        undo.undo();
        waitUndoReady();
        verify(QFile::exists(lifeFile) && !QFile::exists(lifeFileDst)
                   && parent.findChildren<KJob *>().isEmpty(),
               "LIFE-01-FIX-04-2: Move -> Undo restores file and frees KJob");

        undo.redo();
        waitUndoReady();
        verify(!QFile::exists(lifeFile) && QFile::exists(lifeFileDst)
                   && parent.findChildren<KJob *>().isEmpty(),
               "LIFE-01-FIX-04-3: Move -> Undo -> Redo restores destination and frees KJob");

        // 4. Repeated Undo/Redo (3 cycles) -> no KJob accumulation
        for (int cycle = 0; cycle < 3; ++cycle) {
            undo.undo();
            waitUndoReady();
            verify(QFile::exists(lifeFile) && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-4a: repeated Undo frees KJob");
            undo.redo();
            waitUndoReady();
            verify(QFile::exists(lifeFileDst) && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-4b: repeated Redo frees KJob");
        }

        // 5. Undo collision / error -> data integrity preserved and failed KJob freed
        createFixture(lifeFile, "COLLISION_BLOCKER");
        undo.undo();
        verify(QTest::qWaitFor([&] {
            drainDeferredDeletes();
            return parent.findChildren<KJob *>().isEmpty();
        }, 10000), "LIFE-01-FIX-04-5: failed Undo (collision) frees KJob");
        verify(fixtureData(lifeFile) == "COLLISION_BLOCKER"
                   && fixtureData(lifeFileDst) == "LIFE_FILE",
               "LIFE-01-FIX-04-5: collision preserves both files without overwrite");

        // 6. Single Directory Move -> Undo -> Redo (if on Btrfs)
        struct statfs btrfsCheck {};
        if (::statfs(QFile::encodeName(a).constData(), &btrfsCheck) == 0
            && static_cast<unsigned long>(btrfsCheck.f_type) == BTRFS_SUPER_MAGIC) {
            const QString lifeDir = a + QStringLiteral("/life_fix_dir");
            const QString lifeDirDst = b + QStringLiteral("/life_fix_dir");
            verify(QDir().mkpath(lifeDir), "LIFE-01-FIX-04-6: setup dir");
            createFixture(lifeDir + QStringLiteral("/inner.txt"), "DIR_IN");
            const int dirCompleted = completed;
            actions.transfer({QUrl::fromLocalFile(lifeDir)}, rootB, Qt::MoveAction);
            waitJob(dirCompleted + 1);
            drainDeferredDeletes();
            verify(QFileInfo::exists(lifeDirDst + QStringLiteral("/inner.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-6a: Single Dir Move leaves 0 live KJobs");

            undo.undo();
            waitUndoReady();
            verify(QFileInfo::exists(lifeDir + QStringLiteral("/inner.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-6b: Single Dir Undo frees KJob");

            undo.redo();
            waitUndoReady();
            verify(QFileInfo::exists(lifeDirDst + QStringLiteral("/inner.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-6c: Single Dir Redo frees KJob");

            // 7. Mixed Batch Move -> Undo -> Redo -> 0 live KJobs
            const QString mixF = a + QStringLiteral("/life_mix_f.txt");
            const QString mixD = a + QStringLiteral("/life_mix_d");
            createFixture(mixF, "MIX_F");
            verify(QDir().mkpath(mixD), "LIFE-01-FIX-04-7: setup mixed");
            createFixture(mixD + QStringLiteral("/m.txt"), "MIX_D");
            const int mixCompleted = completed;
            actions.transfer({QUrl::fromLocalFile(mixF), QUrl::fromLocalFile(mixD)}, rootB, Qt::MoveAction);
            waitJob(mixCompleted + 1);
            drainDeferredDeletes();
            verify(QFileInfo::exists(b + QStringLiteral("/life_mix_f.txt"))
                       && QFileInfo::exists(b + QStringLiteral("/life_mix_d/m.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-7a: Mixed Batch Move leaves 0 live KJobs");

            undo.undo();
            waitUndoReady();
            verify(QFileInfo::exists(mixF) && QFileInfo::exists(mixD + QStringLiteral("/m.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-7b: Mixed Batch Undo frees KJob");

            undo.redo();
            waitUndoReady();
            verify(QFileInfo::exists(b + QStringLiteral("/life_mix_f.txt"))
                       && QFileInfo::exists(b + QStringLiteral("/life_mix_d/m.txt"))
                       && parent.findChildren<KJob *>().isEmpty(),
                   "LIFE-01-FIX-04-7c: Mixed Batch Redo frees KJob");
        }
    }

    // FOLDER-COLLISION-01: Copying a directory onto an existing directory (self-copy or cross-dir)
    // must fail closed without routing into KioStagedTransferJob or leaving *.thispc-part* directories.
    {
        TransferQueueController fcQueue;
        actions.setQueueController(&fcQueue);
        setTransferQueueProductionDispatchConnected(true);

        const QString fcSrcRoot = a + QStringLiteral("/fc_src");
        const QString fcDstRoot = b + QStringLiteral("/fc_dst");
        const QString wtfSrc = fcSrcRoot + QStringLiteral("/WTF");
        const QString wtfDst = fcDstRoot + QStringLiteral("/WTF");
        verify(QDir().mkpath(wtfSrc + QStringLiteral("/Sub"))
                   && QDir().mkpath(wtfDst + QStringLiteral("/Sub")),
               "FOLDER-COLLISION-01: setup artificial WTF trees");
        createFixture(wtfSrc + QStringLiteral("/Config.wtf"), "SRC_CFG");
        createFixture(wtfSrc + QStringLiteral("/Sub/new_only.txt"), "NEW_ONLY");
        createFixture(wtfDst + QStringLiteral("/Config.wtf"), "DST_CFG");
        createFixture(wtfDst + QStringLiteral("/Sub/dst_only.txt"), "DST_ONLY");

        auto hasStagingArtifact = [](const QString &dirPath) {
            const QStringList entries = QDir(dirPath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
            for (const QString &e : entries) {
                if (e.contains(QStringLiteral(".thispc-part"))) return true;
            }
            return false;
        };

        // 1. Self-copy: copy WTF into its own parent fcSrcRoot
        lastFenceMessage.clear();
        actions.transfer({QUrl::fromLocalFile(wtfSrc)}, QUrl::fromLocalFile(fcSrcRoot), Qt::CopyAction);
        verify(!lastFenceMessage.isEmpty()
                   && fcQueue.snapshot().tasks.isEmpty()
                   && !hasStagingArtifact(fcSrcRoot)
                   && fixtureData(wtfSrc + QStringLiteral("/Config.wtf")) == "SRC_CFG",
               "FOLDER-COLLISION-01-1: self-copy of directory refused fail-closed with 0 .thispc-part artifacts");

        // 2. Cross-directory collision: copy WTF into fcDstRoot where WTF already exists -> routes to Folder Merge
        {
            TransferQueueController mergeQueue;
            actions.setQueueController(&mergeQueue);
            lastFenceMessage.clear();
            actions.transfer({QUrl::fromLocalFile(wtfSrc)}, QUrl::fromLocalFile(fcDstRoot), Qt::CopyAction);
            const TransferQueueSnapshot mergeSnap = mergeQueue.snapshot();
            verify(lastFenceMessage.isEmpty()
                       && mergeSnap.tasks.size() == 1
                       && mergeSnap.tasks.first().type == TransferOperationType::Copy
                       && mergeSnap.tasks.first().backend == TransferBackendKind::LocalNative
                       && mergeSnap.tasks.first().destination == QUrl::fromLocalFile(fcDstRoot)
                       && !hasStagingArtifact(fcDstRoot)
                       && fixtureData(wtfDst + QStringLiteral("/Config.wtf")) == "DST_CFG"
                       && fixtureData(wtfDst + QStringLiteral("/Sub/dst_only.txt")) == "DST_ONLY",
                   "FOLDER-COLLISION-01-2: cross-dir folder collision routes to Folder Merge with 0 .thispc-part artifacts");
            mergeQueue.cancelAll();
            actions.setQueueController(&fcQueue);
        }

        // 3. Direct KioStagedTransferJob defense-in-depth rejects local directory before creating .thispc-part
        auto *dirStaged = new KioStagedTransferJob(
            {QUrl::fromLocalFile(wtfSrc)}, QUrl::fromLocalFile(fcDstRoot), false,
            QUrl(), false, {}, &parent, &parent);
        verify(!dirStaged->exec()
                   && dirStaged->error() == KIO::ERR_IS_DIRECTORY
                   && !hasStagingArtifact(fcDstRoot),
               "FOLDER-COLLISION-01-3: KioStagedTransferJob rejects local directory without staging artifacts");
        delete dirStaged;

        // 3b. Remote KIO directory (MTP/SFTP/SMB) rejected before creating .thispc-part
        auto *remoteDirStaged = new KioStagedTransferJob(
            {QUrl(QStringLiteral("sftp://host/remote/WTF"))}, QUrl::fromLocalFile(fcDstRoot), false,
            QUrl(), false, {}, &parent, &parent);
        remoteDirStaged->setSourceStatProvider([](const QUrl &, bool) {
            KioStagedTransferJob::RemoteSourceMetadata meta;
            meta.isDirectory = true;
            meta.name = QStringLiteral("WTF");
            return meta;
        });
        verify(!remoteDirStaged->exec()
                   && remoteDirStaged->error() == KIO::ERR_IS_DIRECTORY
                   && !hasStagingArtifact(fcDstRoot),
               "FOLDER-COLLISION-01-3b: KioStagedTransferJob rejects remote KIO directory without staging artifacts");
        delete remoteDirStaged;

        // 4. Copying a regular file onto existing regular file still invokes conflict dialog & overwrites cleanly
        actions.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return KIO::Result_Overwrite;
        });
        actions.transfer(
            {QUrl::fromLocalFile(wtfSrc + QStringLiteral("/Config.wtf"))},
            QUrl::fromLocalFile(wtfDst),
            Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = fcQueue.snapshot();
            return snap.tasks.size() == 1
                && snap.tasks.at(0).state == TransferTaskState::Completed;
        }, 10000), "FOLDER-COLLISION-01-4: queued regular file copy with conflict override completes");
        verify(fixtureData(wtfDst + QStringLiteral("/Config.wtf")) == "SRC_CFG"
                   && !hasStagingArtifact(wtfDst),
               "FOLDER-COLLISION-01-4: regular file copy with conflict override still succeeds");
        actions.setConflictDecisionOverride({});

        // C1: Self-copy of regular file into same directory offers Rename (no Overwrite) and copies to Test (1).txt
        auto sha256Of = [&](const QString &p) {
            return QCryptographicHash::hash(fixtureData(p), QCryptographicHash::Sha256);
        };
        const QString c1File = fcSrcRoot + QStringLiteral("/Test.txt");
        const QString c1Copy = fcSrcRoot + QStringLiteral("/Test (1).txt");
        createFixture(c1File, "C1_ORIGINAL_PAYLOAD_2026");
        const QByteArray c1OrigHash = sha256Of(c1File);

        bool c1SawDialog = false;
        QString c1SuggestedText;
        QTimer::singleShot(0, &parent, [&] {
            if (auto *dlg = qobject_cast<KIO::RenameDialog *>(QApplication::activeModalWidget())) {
                c1SawDialog = true;
                if (auto *edit = dlg->findChild<QLineEdit *>()) {
                    c1SuggestedText = edit->text();
                }
                dlg->renamePressed();
            }
        });
        actions.transfer({QUrl::fromLocalFile(c1File)}, QUrl::fromLocalFile(fcSrcRoot), Qt::CopyAction);
        verify(c1SawDialog && c1SuggestedText == QStringLiteral("Test (1).txt"),
               "C1-1: self-copy opens RenameDialog with suggested name Test (1).txt");
        verify(QTest::qWaitFor([&] {
            const auto snap = fcQueue.snapshot();
            return snap.tasks.size() == 2
                && snap.tasks.at(1).state == TransferTaskState::Completed;
        }, 10000), "C1-2: self-copy under new name completes");
        verify(sha256Of(c1File) == c1OrigHash && sha256Of(c1Copy) == c1OrigHash
                   && !hasStagingArtifact(fcSrcRoot),
               "C1-3: original and Test (1).txt have identical SHA-256 and 0 .thispc-part artifacts");

        // C1 hardlink / alias: Result_Overwrite must be refused fail-closed when source and target share inode
        const QString hlDir = a + QStringLiteral("/fc_hl_dir");
        verify(QDir().mkpath(hlDir)
                   && ::link(QFile::encodeName(c1File).constData(),
                             QFile::encodeName(hlDir + QStringLiteral("/Test.txt")).constData()) == 0,
               "C1-hl: setup hardlink");
        actions.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return KIO::Result_Overwrite;
        });
        actions.transfer({QUrl::fromLocalFile(c1File)}, QUrl::fromLocalFile(hlDir), Qt::CopyAction);
        verify(fcQueue.snapshot().tasks.size() == 2 && sha256Of(c1File) == c1OrigHash
                   && !hasStagingArtifact(hlDir),
               "C1-hl: hardlink self-copy refuses Result_Overwrite fail-closed");
        actions.setConflictDecisionOverride({});

        // C2: Two distinct files with same name -> Overwrite, Skip, Rename, Cancel all preserve SHA-256 semantics
        const QString c2Src = fcSrcRoot + QStringLiteral("/C2.txt");
        const QString c2Dst = fcDstRoot + QStringLiteral("/C2.txt");
        createFixture(c2Src, "C2_SOURCE_DATA_AAA");
        createFixture(c2Dst, "C2_DEST_DATA_BBB");
        const QByteArray c2SrcHash = sha256Of(c2Src);
        const QByteArray c2DstHash = sha256Of(c2Dst);

        // C2-Skip & C2-Cancel
        for (int dec : {static_cast<int>(KIO::Result_Skip), static_cast<int>(KIO::Result_Cancel)}) {
            actions.setConflictDecisionOverride([dec](const QUrl &, const QUrl &, QUrl *) {
                return dec;
            });
            actions.transfer({QUrl::fromLocalFile(c2Src)}, QUrl::fromLocalFile(fcDstRoot), Qt::CopyAction);
            verify(fcQueue.snapshot().tasks.size() == 2
                       && sha256Of(c2Dst) == c2DstHash
                       && !hasStagingArtifact(fcDstRoot),
                   "C2-Skip/Cancel: destination SHA-256 unchanged");
        }
        // C2-Rename
        const QString c2Renamed = fcDstRoot + QStringLiteral("/C2_renamed.txt");
        actions.setConflictDecisionOverride([&](const QUrl &, const QUrl &, QUrl *newUrl) {
            *newUrl = QUrl::fromLocalFile(c2Renamed);
            return KIO::Result_Rename;
        });
        actions.transfer({QUrl::fromLocalFile(c2Src)}, QUrl::fromLocalFile(fcDstRoot), Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = fcQueue.snapshot();
            return snap.tasks.size() == 3
                && snap.tasks.at(2).state == TransferTaskState::Completed;
        }, 10000), "C2-Rename: completes");
        verify(sha256Of(c2Dst) == c2DstHash && sha256Of(c2Renamed) == c2SrcHash,
               "C2-Rename: destination preserved and renamed copy matches source SHA-256");

        // C2-Overwrite
        actions.setConflictDecisionOverride([](const QUrl &, const QUrl &, QUrl *) {
            return KIO::Result_Overwrite;
        });
        actions.transfer({QUrl::fromLocalFile(c2Src)}, QUrl::fromLocalFile(fcDstRoot), Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = fcQueue.snapshot();
            return snap.tasks.size() == 4
                && snap.tasks.at(3).state == TransferTaskState::Completed;
        }, 10000), "C2-Overwrite: completes");
        verify(sha256Of(c2Dst) == c2SrcHash && !hasStagingArtifact(fcDstRoot),
               "C2-Overwrite: destination updated to source SHA-256");
        actions.setConflictDecisionOverride({});

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    // =========================================================================
    // 0.45.1 Multi-Agent Remediation (R1/R5/R6 & Production Routing)
    // =========================================================================
    {
        TransferQueueController remQueue;
        actions.setQueueController(&remQueue);
        undo.setQueueController(&remQueue);
        setTransferQueueProductionDispatchConnected(true);

        const QString remRoot = files.filePath(QStringLiteral("rem0451"));
        const QString remDst = remRoot + QStringLiteral("/Dst");
        const QString remOut = remRoot + QStringLiteral("/Outside");
        verify(QDir().mkpath(remDst) && QDir().mkpath(remOut), "REM-0451: roots created");
        createFixture(remOut + QStringLiteral("/secret.txt"), "OUTSIDE-SECRET");

        // R1 + R6: Non-conflicting directory Copy with mixed symlinks routes through LocalTransferJob,
        // preserves symlinks without dereferencing, AND remains Ctrl+Z reversible via UndoController.
        const QString cleanDir = remRoot + QStringLiteral("/CleanSymDir");
        verify(QDir().mkpath(cleanDir + QStringLiteral("/Sub")), "REM-R1-R6: clean dir created");
        createFixture(cleanDir + QStringLiteral("/reg.txt"), "REG-DATA");
        createFixture(cleanDir + QStringLiteral("/Sub/sub.txt"), "SUB-DATA");
        verify(::symlink("reg.txt", QFile::encodeName(cleanDir + QStringLiteral("/rel.lnk")).constData()) == 0
                   && ::symlink(QFile::encodeName(cleanDir + QStringLiteral("/reg.txt")).constData(),
                                QFile::encodeName(cleanDir + QStringLiteral("/abs.lnk")).constData()) == 0
                   && ::symlink(QFile::encodeName(remOut).constData(),
                                QFile::encodeName(cleanDir + QStringLiteral("/dir.lnk")).constData()) == 0
                   && ::symlink("no_such_target",
                                QFile::encodeName(cleanDir + QStringLiteral("/broken.lnk")).constData()) == 0,
               "REM-R1-R6: symlinks created");

        actions.transfer({QUrl::fromLocalFile(cleanDir)}, QUrl::fromLocalFile(remDst), Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = remQueue.snapshot();
            return snap.tasks.size() == 1 && snap.tasks.at(0).state == TransferTaskState::Completed;
        }, 10000), "REM-R1-R6: non-conflicting dir copy with symlinks completes");

        const QString copiedCleanDir = remDst + QStringLiteral("/CleanSymDir");
        struct stat stCopiedDirLnk {};
        verify(fixtureData(copiedCleanDir + QStringLiteral("/reg.txt")) == "REG-DATA"
                   && fixtureData(copiedCleanDir + QStringLiteral("/Sub/sub.txt")) == "SUB-DATA"
                   && readLocalLink(copiedCleanDir + QStringLiteral("/rel.lnk")) == "reg.txt"
                   && readLocalLink(copiedCleanDir + QStringLiteral("/abs.lnk")) == QFile::encodeName(cleanDir + QStringLiteral("/reg.txt"))
                   && readLocalLink(copiedCleanDir + QStringLiteral("/dir.lnk")) == QFile::encodeName(remOut)
                   && readLocalLink(copiedCleanDir + QStringLiteral("/broken.lnk")) == "no_such_target"
                   && ::lstat(QFile::encodeName(copiedCleanDir + QStringLiteral("/dir.lnk")).constData(), &stCopiedDirLnk) == 0
                   && S_ISLNK(stCopiedDirLnk.st_mode),
               "REM-R1: non-conflicting directory copy preserves all 4 symlinks without dereferencing");
        verify(undo.treeUndoAvailable() && undoAction.isEnabled(),
               "REM-R6: Ctrl+Z Undo remains available after non-conflicting directory copy");
        undo.undo();
        verify(QTest::qWaitFor([&] {
            return !undo.m_busy && !QFileInfo::exists(copiedCleanDir);
        }, 10000), "REM-R6: Ctrl+Z Undo cleanly removes non-conflicting copied directory");

        // R1: Standalone symlink-to-directory Copy is preserved as a symlink (not dereferenced or merged)
        const QString soloLink = remRoot + QStringLiteral("/solo_dir.lnk");
        verify(::symlink(QFile::encodeName(remOut).constData(), QFile::encodeName(soloLink).constData()) == 0,
               "REM-R1-solo: standalone symlink created");
        actions.transfer({QUrl::fromLocalFile(soloLink)}, QUrl::fromLocalFile(remDst), Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = remQueue.snapshot();
            return snap.tasks.size() == 2 && snap.tasks.at(1).state == TransferTaskState::Completed;
        }, 10000), "REM-R1-solo: standalone symlink-to-dir copy completes");
        struct stat stSoloCopied {};
        verify(::lstat(QFile::encodeName(remDst + QStringLiteral("/solo_dir.lnk")).constData(), &stSoloCopied) == 0
                   && S_ISLNK(stSoloCopied.st_mode)
                   && readLocalLink(remDst + QStringLiteral("/solo_dir.lnk")) == QFile::encodeName(remOut),
               "REM-R1-solo: standalone symlink-to-dir preserved as symlink without dereference");

        // R6: Conflicting directory Copy routes to Folder Merge and fences UndoController
        const QString mergeSrc = remRoot + QStringLiteral("/MergeDir");
        const QString mergeDstExisting = remDst + QStringLiteral("/MergeDir");
        verify(QDir().mkpath(mergeSrc) && QDir().mkpath(mergeDstExisting), "REM-R6-fence: merge dirs created");
        createFixture(mergeSrc + QStringLiteral("/new.txt"), "NEW-MERGED");
        createFixture(mergeDstExisting + QStringLiteral("/keep.txt"), "KEEP-EXISTING");
        verify(::symlink("new.txt", QFile::encodeName(mergeSrc + QStringLiteral("/m.lnk")).constData()) == 0,
               "REM-R6-fence: symlink in merge source created");

        const quint64 serialBeforeMerge = undo.irreversibleSerial();
        bool sawFolderDlg = false;
        QTimer autoMergeTimer;
        autoMergeTimer.setInterval(10);
        QObject::connect(&autoMergeTimer, &QTimer::timeout, &parent, [&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *dlg = dynamic_cast<LocalFolderMergeConflictDialog *>(w); dlg && dlg->isVisible()) {
                    sawFolderDlg = true;
                    dlg->mergeButton()->click();
                    break;
                }
            }
        });
        autoMergeTimer.start();
        actions.transfer({QUrl::fromLocalFile(mergeSrc)}, QUrl::fromLocalFile(remDst), Qt::CopyAction);
        verify(QTest::qWaitFor([&] {
            const auto snap = remQueue.snapshot();
            return snap.tasks.size() == 3 && snap.tasks.at(2).state == TransferTaskState::Completed;
        }, 10000), "REM-R6-fence: colliding directory copy completes via Folder Merge");
        autoMergeTimer.stop();
        verify(sawFolderDlg
                   && fixtureData(mergeDstExisting + QStringLiteral("/new.txt")) == "NEW-MERGED"
                   && fixtureData(mergeDstExisting + QStringLiteral("/keep.txt")) == "KEEP-EXISTING"
                   && readLocalLink(mergeDstExisting + QStringLiteral("/m.lnk")) == "new.txt"
                   && undo.irreversibleSerial() > serialBeforeMerge,
               "REM-R6-fence: Folder Merge merged regular file + symlink, preserved existing file, and fenced UndoController");

        // R5: "Send To" (copySelectionToDirectory) routes colliding local directory through Folder Merge & emits KDirNotify
        const QString sendRoot = remRoot + QStringLiteral("/SendToDst");
        const QString sendSrc = remRoot + QStringLiteral("/SendFolder");
        const QString sendDstSub = sendRoot + QStringLiteral("/SendFolder");
        verify(QDir().mkpath(sendSrc) && QDir().mkpath(sendDstSub), "REM-R5: Send To fixtures created");
        createFixture(sendSrc + QStringLiteral("/sent.txt"), "SENT-PAYLOAD");
        createFixture(sendDstSub + QStringLiteral("/old.txt"), "OLD-SEND-DST");

        org::kde::KDirNotify dirNotifySpy(QString(), QString(), QDBusConnection::sessionBus(), &parent);
        QSignalSpy filesAddedSpy(&dirNotifySpy, &org::kde::KDirNotify::FilesAdded);
        bool sawSendMergeDlg = false;
        QTimer autoSendTimer;
        autoSendTimer.setInterval(10);
        QObject::connect(&autoSendTimer, &QTimer::timeout, &parent, [&] {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (auto *dlg = dynamic_cast<LocalFolderMergeConflictDialog *>(w); dlg && dlg->isVisible()) {
                    sawSendMergeDlg = true;
                    dlg->mergeButton()->click();
                    break;
                }
            }
        });
        autoSendTimer.start();
        actions.copySelectionToDirectory({QUrl::fromLocalFile(sendSrc)}, sendRoot, QStringLiteral("Send done"));
        verify(QTest::qWaitFor([&] {
            const auto snap = remQueue.snapshot();
            return snap.tasks.size() == 4 && snap.tasks.at(3).state == TransferTaskState::Completed;
        }, 10000), "REM-R5: Send To routes colliding folder through Folder Merge and completes");
        autoSendTimer.stop();
        verify(QTest::qWaitFor([&] {
            for (const QList<QVariant> &args : std::as_const(filesAddedSpy)) {
                if (!args.isEmpty() && args.first().toString().contains(QStringLiteral("SendToDst"))) {
                    return true;
                }
            }
            return false;
        }, 5000), "REM-R5: Folder Merge emits KDirNotify::FilesAdded for destination view refresh");
        verify(sawSendMergeDlg
                   && fixtureData(sendDstSub + QStringLiteral("/sent.txt")) == "SENT-PAYLOAD"
                   && fixtureData(sendDstSub + QStringLiteral("/old.txt")) == "OLD-SEND-DST",
               "REM-R5: Send To merged colliding directory without legacy fallback");

        // Production Routing safety: isIntoSelf (even without top-level collision) & isSelfCopy blocked fail-closed
        const QString selfParent = remRoot + QStringLiteral("/SelfDir");
        const QString selfChild = selfParent + QStringLiteral("/SubIntoSelf");
        verify(QDir().mkpath(selfChild), "REM-SELF: cycle dirs created");
        lastFenceMessage.clear();
        actions.transfer({QUrl::fromLocalFile(selfParent)}, QUrl::fromLocalFile(selfChild), Qt::CopyAction);
        verify(!lastFenceMessage.isEmpty() && remQueue.snapshot().tasks.size() == 4
                   && !QFileInfo::exists(selfChild + QStringLiteral("/SelfDir")),
               "REM-SELF: copying directory into its own subdirectory refused fail-closed without queue task");

        lastFenceMessage.clear();
        actions.transfer({QUrl::fromLocalFile(selfParent)}, QUrl::fromLocalFile(remRoot), Qt::CopyAction);
        verify(!lastFenceMessage.isEmpty() && remQueue.snapshot().tasks.size() == 4,
               "REM-SELF: self-copy into same parent refused fail-closed without queue task");

        // FileActions::detectLocalMergeCapabilities on local filesystem
        const LocalMergeBackendCapabilities caps = FileActions::detectLocalMergeCapabilities(remDst);
        verify(caps.canMergeDirectories && caps.canRenameConflictingCopies && caps.canSafeOverwrite,
               "REM-CAPS: detectLocalMergeCapabilities reports safe overwrite/rename/merge on local temp fs");

        setTransferQueueProductionDispatchConnected(false);
        undo.setQueueController(nullptr);
        actions.setQueueController(nullptr);
    }

    qInfo("PASS: %d FileActions assertions; native local copy/move, KIO fallbacks, conflicts, Undo/Redo", checks);
}
