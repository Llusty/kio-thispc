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

    // Scenario G: Multiple regular files move refusal before modification
    {
        const QString fileG1 = files.filePath("multi-g1.txt");
        const QString fileG2 = files.filePath("multi-g2.txt");
        createFixture(fileG1, "g1");
        createFixture(fileG2, "g2");
        auto *cutMimeG = new QMimeData;
        cutMimeG->setUrls({QUrl::fromLocalFile(fileG1), QUrl::fromLocalFile(fileG2)});
        cutMimeG->setData(QStringLiteral("application/x-kde-cutselection"), QByteArrayLiteral("1"));
        app.clipboard()->setMimeData(cutMimeG);

        lastFenceMessage.clear();
        const int beforeG = completed;
        actions.pasteClipboardInto(rootB);
        verify(completed == beforeG && QFile::exists(fileG1) && QFile::exists(fileG2)
                   && !QFile::exists(b + "/multi-g1.txt") && !QFile::exists(b + "/multi-g2.txt")
                   && lastFenceMessage == moveContainmentErrorMessage(),
               "Scenario G: multiple files move refused fail-closed before any modification");
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

        // Calling startNativeSingleFileTransfer directly with multiple files must NOT dispatch LocalTransferJob(move=true)
        const int beforeCompD = completed;
        lastFenceMessage.clear();
        const bool handledMulti = actions.startNativeSingleFileTransfer(
            {QUrl::fromLocalFile(dSrc), QUrl::fromLocalFile(files.filePath("btrfs-single-src.txt"))},
            rootB, true, false, {}, {});
        verify(handledMulti && completed == beforeCompD && lastFenceMessage == moveContainmentErrorMessage(),
               "H.1.1-D: direct startNativeSingleFileTransfer multi-move blocked fail-closed");

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
        actions.setQueueController(nullptr);
    }

    qInfo("PASS: %d FileActions assertions; native local copy/move, KIO fallbacks, conflicts, Undo/Redo", checks);
}
