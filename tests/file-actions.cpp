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
    QWidget parent;
    QAction undoAction(&parent), redoAction(&parent);
    UndoController undo(&parent, [] {}, [] {}, [](const QString &, int) {});
    undo.setActions(&undoAction, &redoAction);
    int completed = 0, lastError = 0;
    bool lastClear = false, interactive = true;
    FileActions actions(&parent, &undo, [&](KJob *job, const QString &, bool clear, const QString &) {
        lastClear = clear;
        if (auto *copy = qobject_cast<KIO::CopyJob *>(job)) {
            verify(interactive ? copy->uiDelegate() != nullptr : copy->uiDelegate() == nullptr,
                   "operation has the expected delegate");
            if (interactive)
                verify(!copy->uiDelegate()->isAutoErrorHandlingEnabled(), "no duplicate automatic error UI");
        }
        QObject::connect(job, &KJob::result, &parent, [&](KJob *done) {
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
        waitJob(expected, !cancel);
        verify(conflictSeen, "KIO conflict UI remains interactive");
        verify(QFile::exists(a + "/renamed.txt") && QFile::exists(b + "/renamed.txt"), "conflict preserves expected files");
    }
    auto *mime = new QMimeData;
    mime->setUrls({source});
    mime->setData("application/x-kde-cutselection", "1");
    app.clipboard()->setMimeData(mime);
    actions.pasteClipboardInto(QUrl::fromLocalFile(a + "/folder"));
    waitJob(7);
    verify(lastClear, "cut requests clipboard cleanup only on success");
    verify(!QFile::exists(a + "/renamed.txt") && QFile::exists(a + "/folder/renamed.txt"), "clipboard cut moves source");
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
    waitJob(8);
    verify(!QFile::exists(trashed), "disposable file moved to Trash");
    undoRedo([&] { return QFile::exists(trashed); }, [&] { return !QFile::exists(trashed); });
    undo.undo();
    verify(QTest::qWaitFor([&] { return !undo.m_busy && QFile::exists(trashed); }, 10000),
           "restore disposable file from Trash before cleanup");
    qInfo("PASS: %d FileActions assertions; real KIO create/copy/move/rename/Trash, conflicts, Undo/Redo", checks);
}
