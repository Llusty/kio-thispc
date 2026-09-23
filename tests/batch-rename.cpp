// Appended to a temporary, instrumented copy by run-pane-actions.py.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto writeBarrier = [](const QString &path, const QByteArray &value) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            && file.write(value) == value.size() && file.flush();
    };
    const auto waitBarrier = [](const QString &path) {
        QElapsedTimer timer;
        timer.start();
        while (!QFileInfo::exists(path) && timer.elapsed() < 10000) QThread::msleep(5);
        return QFileInfo::exists(path);
    };
    if (argc > 7 && QByteArray(argv[2]) == QByteArrayLiteral("--stage1c-audit-worker")) {
        const QString root = QString::fromLocal8Bit(argv[3]);
        const QString ready = QString::fromLocal8Bit(argv[4]);
        const QString release = QString::fromLocal8Bit(argv[5]);
        const QString effect = QString::fromLocal8Bit(argv[6]);
        BatchRenameRecoveryGate gate(root);
        const bool allowed = !gate.mutationsBlocked();
        if (!writeBarrier(ready, allowed ? QByteArrayLiteral("clean") : QByteArrayLiteral("blocked"))) return 10;
        if (!waitBarrier(release)) return 11;
        if (allowed && !writeBarrier(effect, QByteArrayLiteral("dispatched"))) return 12;
        return allowed ? 0 : 3;
    }
    if (argc > 8 && QByteArray(argv[2]) == QByteArrayLiteral("--stage1c-journal-worker")) {
        const QString root = QString::fromLocal8Bit(argv[3]);
        const QString ready = QString::fromLocal8Bit(argv[4]);
        const QString release = QString::fromLocal8Bit(argv[5]);
        const QUrl first = QUrl::fromLocalFile(QString::fromLocal8Bit(argv[6]));
        const QUrl second = QUrl::fromLocalFile(QString::fromLocal8Bit(argv[7]));
        const BatchRenamePlan plan = makeBatchRenamePlan(
            {first, second}, QStringLiteral("stage1c-"), {}, false, 1, 2);
        BatchRenameRecoveryJournal journal(root);
        QString error;
        const bool published = plan.isValid() && journal.beginLinear(plan, false, &error);
        if (!writeBarrier(ready, published ? QByteArrayLiteral("published") : QByteArrayLiteral("blocked"))) return 20;
        if (!waitBarrier(release)) return 21;
        if (published && !journal.finishLinear(QStringLiteral("verified-test"), 0, &error)) return 22;
        return published ? 0 : 3;
    }
    if (argc > 3 && QByteArray(argv[2]) == QByteArrayLiteral("--probe-recovery-lock")) {
        BatchRenameRecoveryGate probe(QString::fromLocal8Bit(argv[3]));
        return probe.audit().writerLock == BatchRenameRecoveryLock::Live ? 0 : 3;
    }
    if (argc > 4 && QByteArray(argv[2]) == QByteArrayLiteral("--stage2-v2-kill-worker")) {
        const QString root = QString::fromLocal8Bit(argv[3]);
        const QString directory = QString::fromLocal8Bit(argv[4]);
        QList<QUrl> sources;
        for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                    QStringLiteral("2"), QStringLiteral("42")})
            sources.append(QUrl::fromLocalFile(QDir(directory).filePath(name)));
        const auto plan = makeBatchRenamePlan(sources, QStringLiteral("4"), {}, false, 1, 2);
        if (!batchRenameLinearHistoryEligible(plan)) return 31;
        const auto result = batchRenameExecuteLinearV2(plan, root);
        return result.success ? 0 : 32;
    }
    if (argc > 5 && QByteArray(argv[2]) == QByteArrayLiteral("--stage3-v2-history-kill-worker")) {
        const QString root = QString::fromLocal8Bit(argv[3]);
        const QString directory = QString::fromLocal8Bit(argv[4]);
        const bool undo = QByteArray(argv[5]) == QByteArrayLiteral("undo");
        const QByteArray killMarker = qgetenv("THISPC_RECOVERY_KILL_AT");
        qunsetenv("THISPC_RECOVERY_KILL_AT");
        qputenv("THISPC_RECOVERY_ROOT", QFile::encodeName(root));
        auto &startupGate = BatchRenameRecoveryGate::instance();
        if (startupGate.beginStartupScan()) startupGate.completeStartupScan();
        QList<QUrl> sources;
        for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                    QStringLiteral("2"), QStringLiteral("42")})
            sources.append(QUrl::fromLocalFile(QDir(directory).filePath(name)));
        const auto plan = makeBatchRenamePlan(sources, QStringLiteral("4"), {}, false, 1, 2);
        if (!batchRenameLinearHistoryEligible(plan)) return 41;
        for (int row : plan.executionOrder) {
            QString setupError;
            if (!batchRenameLinearMove(plan.entries.at(row), false, &setupError)) return 411;
        }
        if (!batchRenameLinearMappingMatches(plan, true, 0)) return 412;
        UndoController history(nullptr, [](bool) {}, [] {}, [](const QString &, int) {});
        QAction undoAction;
        QAction redoAction;
        history.setActions(&undoAction, &redoAction);
        if (!history.m_manager || !history.recordCompletedLinear(plan)
            || !undoAction.isEnabled()) return 42;
        if (!undo) {
            history.undo();
            QElapsedTimer timer;
            timer.start();
            while (!redoAction.isEnabled() && timer.elapsed() < 10000)
                QApplication::processEvents(QEventLoop::AllEvents, 10);
            if (!redoAction.isEnabled()) return 43;
        }
        qputenv("THISPC_RECOVERY_KILL_AT", killMarker);
        if (undo) history.undo();
        else history.redo();
        QTimer::singleShot(10000, &app, &QCoreApplication::quit);
        return app.exec() == 0 ? 44 : 45;
    }
    if (argc > 3 && QByteArray(argv[2]) == QByteArrayLiteral("--stage2-recovery-worker")) {
        BatchRenameRecoveryGate recovery(QString::fromLocal8Bit(argv[3]));
        return recovery.mutationsBlocked() ? 33 : 0;
    }
    if (argc > 5 && QByteArray(argv[2]) == QByteArrayLiteral("--stage4a1-swap-worker")) {
        const QString root = QString::fromLocal8Bit(argv[3]);
        const QString directory = QString::fromLocal8Bit(argv[4]);
        const QString direction = QString::fromLocal8Bit(argv[5]);
        BatchRenameRecoveryGate protocol(root); // recovery-audit flock is always first
        QLockFile cycle(QDir(root).filePath(QStringLiteral("cycle.lock")));
        if (!protocol.ownsProtocolLock() || !cycle.tryLock(0)) return 51;
        QString path = BatchRenameRecoveryJournal::pendingJournal(root);
        if (path.isEmpty() && argc > 6 && QByteArray(argv[6]) == QByteArrayLiteral("recover")) return 0;
        if (path.isEmpty()) {
            batchRenameRecoveryFaultPoint("before-prepared");
            QJsonObject object;
            object.insert(QStringLiteral("schema"), 2);
            object.insert(QStringLiteral("scope"), QStringLiteral("recovery-audit-v2"));
            object.insert(QStringLiteral("kind"), QStringLiteral("swap"));
            object.insert(QStringLiteral("directory"), directory);
            object.insert(QStringLiteral("direction"), direction);
            object.insert(QStringLiteral("phase"), QStringLiteral("prepared"));
            object.insert(QStringLiteral("completedSteps"), 0);
            struct stat ds {};
            if (::lstat(QFile::encodeName(directory).constData(), &ds) != 0) return 52;
            object.insert(QStringLiteral("directoryDevice"), QString::number(quint64(ds.st_dev)));
            object.insert(QStringLiteral("directoryInode"), QString::number(quint64(ds.st_ino)));
            QJsonArray items;
            const QStringList names{QStringLiteral("left"), QStringLiteral("right")};
            for (int i = 0; i < 2; ++i) {
                const QString source = QDir(directory).filePath(names.at(i));
                struct stat st {};
                if (::lstat(QFile::encodeName(source).constData(), &st) != 0) return 53;
                QJsonObject item;
                item.insert(QStringLiteral("row"), i);
                item.insert(QStringLiteral("source"), source);
                item.insert(QStringLiteral("destination"), QDir(directory).filePath(names.at(1 - i)));
                item.insert(QStringLiteral("device"), QString::number(quint64(st.st_dev)));
                item.insert(QStringLiteral("inode"), QString::number(quint64(st.st_ino)));
                item.insert(QStringLiteral("mode"), QString::number(quint64(st.st_mode)));
                item.insert(QStringLiteral("size"), QString::number(qint64(st.st_size)));
                item.insert(QStringLiteral("mtimeNs"), QString::number(
                    qint64(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec));
                item.insert(QStringLiteral("type"), S_ISLNK(st.st_mode) ? QStringLiteral("symlink")
                    : (S_ISDIR(st.st_mode) ? QStringLiteral("directory") : QStringLiteral("file")));
                items.append(item);
            }
            object.insert(QStringLiteral("items"), items);
            object.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(object));
            path = QDir(root).filePath(QStringLiteral("swap-test.json"));
            QSaveFile output(path);
            output.setDirectWriteFallback(false);
            const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()
                || !output.commit()) return 54;
            const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0 || ::fsync(fd) != 0) { if (fd >= 0) ::close(fd); return 55; }
            ::close(fd);
            const int rootfd = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (rootfd < 0 || ::fsync(rootfd) != 0) { if (rootfd >= 0) ::close(rootfd); return 56; }
            ::close(rootfd);
            batchRenameRecoveryFaultPoint("after-prepared");
        }
        const auto entry = parseBatchRenameRecoveryJournal(path);
        QString error;
        return BatchRenameRecoveryDetail::recoverSwapV2(entry, &error) ? 0 : 57;
    }
    QTemporaryDir processRecoveryRoot;
    verify(processRecoveryRoot.isValid(), "isolated process-wide recovery root");
    qputenv("THISPC_RECOVERY_ROOT", QFile::encodeName(processRecoveryRoot.path()));
    QTemporaryDir files;
    verify(files.isValid(), "temporary directory");
    const auto makeFile = [&](const QString &name) {
        QFile file(files.filePath(name));
        verify(file.open(QIODevice::WriteOnly), "fixture opens");
        verify(file.write(name.toUtf8()) == name.toUtf8().size(), "fixture writes");
        file.close();
        return QUrl::fromLocalFile(file.fileName());
    };
    const QUrl beta = makeFile(QStringLiteral("beta.txt"));
    const QUrl alpha = makeFile(QStringLiteral("alpha.txt"));

    const QList<QByteArray> v2KillMarkers{
        "before-prepared", "after-prepared", "before-intent", "after-intent",
        "before-syscall", "after-syscall", "before-data-dir-fsync",
        "after-data-dir-fsync", "after-verified", "after-last-rename-before-goal",
        "after-goal-before-unlink", "after-unlink-before-journal-dir-fsync"};
    const QList<QByteArray> swapKillMarkers{
        "after-prepared", "before-intent", "after-intent", "before-syscall",
        "after-syscall", "before-data-dir-fsync", "after-data-dir-fsync",
        "after-postcheck", "after-verified", "after-goal-before-unlink"};
    const auto runSwapWorker = [&](const QString &root, const QString &data,
                                   const QByteArray &direction,
                                   const QProcessEnvironment &environment) {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
            QStringLiteral("--stage4a1-swap-worker"), root, data,
            QString::fromLatin1(direction), QStringLiteral("recover")});
        verify(process.waitForStarted() && process.waitForFinished(10000),
               "isolated swap recovery helper finishes or crashes deterministically");
        return qMakePair(process.exitStatus(), process.exitCode());
    };
    const auto makeSwapCrashFixture = [&](QTemporaryDir &testCase, const QByteArray &direction,
                                          QString *root, QString *data,
                                          quint64 *leftInode, quint64 *rightInode) {
        *data = testCase.filePath(QStringLiteral("data"));
        *root = testCase.filePath(QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
        if (!QDir().mkpath(*data) || !QDir().mkpath(*root)
            || !QFile::setPermissions(*root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner)) return false;
        for (const auto &pair : {qMakePair(QStringLiteral("left"), QByteArrayLiteral("LEFT")),
                                 qMakePair(QStringLiteral("right"), QByteArrayLiteral("RIGHT"))}) {
            QFile file(QDir(*data).filePath(pair.first));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                || file.write(pair.second) != pair.second.size()) return false;
        }
        struct stat left {}, right {};
        if (::lstat(QFile::encodeName(QDir(*data).filePath(QStringLiteral("left"))).constData(), &left) != 0
            || ::lstat(QFile::encodeName(QDir(*data).filePath(QStringLiteral("right"))).constData(), &right) != 0)
            return false;
        *leftInode = quint64(left.st_ino); *rightInode = quint64(right.st_ino);
        QProcessEnvironment create = QProcessEnvironment::systemEnvironment();
        create.insert(QStringLiteral("XDG_DATA_HOME"), testCase.filePath(QStringLiteral("xdg")));
        create.insert(QStringLiteral("THISPC_RECOVERY_KILL_AT"), QStringLiteral("after-prepared"));
        QProcess process;
        process.setProcessEnvironment(create);
        process.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
            QStringLiteral("--stage4a1-swap-worker"), *root, *data,
            QString::fromLatin1(direction), QStringLiteral("create")});
        return process.waitForStarted() && process.waitForFinished(10000)
            && process.exitStatus() == QProcess::CrashExit
            && !BatchRenameRecoveryJournal::pendingJournal(*root).isEmpty();
    };
    const auto swapAtGoal = [](const QString &data, quint64 leftInode, quint64 rightInode) {
        QFile left(QDir(data).filePath(QStringLiteral("left")));
        QFile right(QDir(data).filePath(QStringLiteral("right")));
        struct stat ls {}, rs {};
        return left.open(QIODevice::ReadOnly) && right.open(QIODevice::ReadOnly)
            && left.readAll() == QByteArrayLiteral("RIGHT")
            && right.readAll() == QByteArrayLiteral("LEFT")
            && ::lstat(QFile::encodeName(left.fileName()).constData(), &ls) == 0
            && ::lstat(QFile::encodeName(right.fileName()).constData(), &rs) == 0
            && quint64(ls.st_ino) == rightInode && quint64(rs.st_ino) == leftInode;
    };
    {
        QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
        verify(testCase.isValid() && makeSwapCrashFixture(testCase, QByteArrayLiteral("forward"),
                                                           &root, &data, &leftInode, &rightInode),
               "isolated strict swap semantic-validator fixture");
        const QString validPath = BatchRenameRecoveryJournal::pendingJournal(root);
        const auto valid = parseBatchRenameRecoveryJournal(validPath);
        verify(valid.kind == BatchRenameRecoveryKind::V2 && valid.items.size() == 2
                   && valid.mapping == BatchRenameRecoveryMapping::Initial,
               "strict validator accepts the exact reciprocal swap plan and before mapping");
        QFile source(validPath);
        verify(source.open(QIODevice::ReadOnly), "valid swap manifest is readable for negative variants");
        const QJsonObject base = QJsonDocument::fromJson(source.readAll()).object();
        source.close();
        QList<QJsonObject> invalid;
        QJsonObject phase = base;
        phase.insert(QStringLiteral("phase"), QStringLiteral("verified-complete"));
        invalid.append(phase);
        QJsonObject steps = base;
        steps.insert(QStringLiteral("completedSteps"), 2);
        steps.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(steps));
        invalid.append(steps);
        QJsonObject phaseSteps = base;
        phaseSteps.insert(QStringLiteral("phase"), QStringLiteral("exchange-verified"));
        phaseSteps.insert(QStringLiteral("completedSteps"), 0);
        phaseSteps.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(phaseSteps));
        invalid.append(phaseSteps);
        QJsonObject direction = base;
        direction.insert(QStringLiteral("direction"), QStringLiteral("sideways"));
        direction.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(direction));
        invalid.append(direction);
        QJsonObject oneItem = base;
        QJsonArray one = oneItem.value(QStringLiteral("items")).toArray(); one.removeLast();
        oneItem.insert(QStringLiteral("items"), one);
        oneItem.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(oneItem));
        invalid.append(oneItem);
        QJsonObject nonReciprocal = base;
        QJsonArray broken = nonReciprocal.value(QStringLiteral("items")).toArray();
        QJsonObject brokenItem = broken.at(1).toObject();
        brokenItem.insert(QStringLiteral("destination"), QDir(data).filePath(QStringLiteral("third")));
        broken.replace(1, brokenItem); nonReciprocal.insert(QStringLiteral("items"), broken);
        nonReciprocal.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(nonReciprocal));
        invalid.append(nonReciprocal);
        QJsonObject duplicateIdentity = base;
        QJsonArray duplicates = duplicateIdentity.value(QStringLiteral("items")).toArray();
        QJsonObject duplicate = duplicates.at(1).toObject();
        const QJsonObject first = duplicates.at(0).toObject();
        duplicate.insert(QStringLiteral("device"), first.value(QStringLiteral("device")));
        duplicate.insert(QStringLiteral("inode"), first.value(QStringLiteral("inode")));
        duplicates.replace(1, duplicate); duplicateIdentity.insert(QStringLiteral("items"), duplicates);
        duplicateIdentity.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(duplicateIdentity));
        invalid.append(duplicateIdentity);
        QJsonObject extraKey = base;
        extraKey.insert(QStringLiteral("unexpected"), true);
        extraKey.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(extraKey));
        invalid.append(extraKey);
        int variant = 0;
        for (const QJsonObject &object : std::as_const(invalid)) {
            const QString path = testCase.filePath(QStringLiteral("invalid-swap-%1.json").arg(++variant));
            QFile file(path); const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
            verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && file.write(bytes) == bytes.size(),
                   "invalid swap semantic variant is written only inside its temporary fixture");
            file.close();
            verify(parseBatchRenameRecoveryJournal(path).kind == BatchRenameRecoveryKind::Corrupt,
                   "swap plans, states, keys, identities and digest fail closed independently");
        }
        const QString symlinkPath = testCase.filePath(QStringLiteral("swap-symlink.json"));
        verify(::symlink(QFile::encodeName(validPath).constData(), QFile::encodeName(symlinkPath).constData()) == 0
                   && parseBatchRenameRecoveryJournal(symlinkPath).kind == BatchRenameRecoveryKind::Corrupt,
               "swap journal symlink is never parsed through its target");
    }
    for (const QByteArray &direction : {QByteArrayLiteral("forward"), QByteArrayLiteral("undo"),
                                        QByteArrayLiteral("redo")}) {
        for (const QByteArray &marker : swapKillMarkers) {
            QTemporaryDir crashCase;
            const QString data = crashCase.filePath(QStringLiteral("data"));
            const QString root = crashCase.filePath(QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
            verify(crashCase.isValid() && QDir().mkpath(data) && QDir().mkpath(root),
                   "isolated swap process fixture");
            verify(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ExeOwner), "private swap recovery root");
            for (const auto &pair : {qMakePair(QStringLiteral("left"), QByteArrayLiteral("LEFT")),
                                     qMakePair(QStringLiteral("right"), QByteArrayLiteral("RIGHT"))}) {
                QFile file(QDir(data).filePath(pair.first));
                verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                           && file.write(pair.second) == pair.second.size(), "swap payload fixture");
            }
            QProcess first;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("XDG_DATA_HOME"), crashCase.filePath(QStringLiteral("xdg")));
            environment.insert(QStringLiteral("THISPC_RECOVERY_KILL_AT"), QString::fromLatin1(marker));
            environment.insert(QStringLiteral("THISPC_RECOVERY_RECOVER_KILL_AT"), QString::fromLatin1(marker));
            first.setProcessEnvironment(environment);
            first.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
                QStringLiteral("--stage4a1-swap-worker"), root, data, QString::fromLatin1(direction),
                QStringLiteral("create")});
            if (!first.waitForStarted() || !first.waitForFinished(10000)
                || first.exitStatus() != QProcess::CrashExit)
                qFatal("FAIL swap marker %s: status=%d code=%d stderr=%s", marker.constData(),
                       int(first.exitStatus()), first.exitCode(), first.readAllStandardError().constData());
            ++checks;
            QProcess recovery;
            environment.remove(QStringLiteral("THISPC_RECOVERY_KILL_AT"));
            environment.remove(QStringLiteral("THISPC_RECOVERY_RECOVER_KILL_AT"));
            recovery.setProcessEnvironment(environment);
            recovery.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
                QStringLiteral("--stage4a1-swap-worker"), root, data, QString::fromLatin1(direction),
                QStringLiteral("recover")});
            verify(recovery.waitForStarted() && recovery.waitForFinished(10000)
                       && recovery.exitStatus() == QProcess::NormalExit && recovery.exitCode() == 0,
                   "swap recovery safely completes recorded direction");
            QFile left(QDir(data).filePath(QStringLiteral("left")));
            QFile right(QDir(data).filePath(QStringLiteral("right")));
            verify(left.open(QIODevice::ReadOnly) && right.open(QIODevice::ReadOnly)
                       && left.readAll() == QByteArrayLiteral("RIGHT")
                       && right.readAll() == QByteArrayLiteral("LEFT")
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "swap recovery preserves both payloads and cleans only at goal");
            QProcess restart;
            restart.setProcessEnvironment(environment);
            restart.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
                QStringLiteral("--stage2-recovery-worker"), root});
            verify(restart.waitForStarted() && restart.waitForFinished(10000)
                       && restart.exitCode() == 0, "second swap restart is clean and idempotent");
        }
    }
    for (const QByteArray &direction : {QByteArrayLiteral("forward"), QByteArrayLiteral("undo"),
                                        QByteArrayLiteral("redo")}) {
        {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated parallel swap recovery fixture");
            const QString ready = testCase.filePath(QStringLiteral("swap-ready"));
            const QString release = testCase.filePath(QStringLiteral("swap-release"));
            QProcessEnvironment held = QProcessEnvironment::systemEnvironment();
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_AT"), QStringLiteral("before-intent"));
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_READY"), ready);
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_RELEASE"), release);
            QProcess owner;
            owner.setProcessEnvironment(held);
            owner.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
                QStringLiteral("--stage4a1-swap-worker"), root, data,
                QString::fromLatin1(direction), QStringLiteral("recover")});
            verify(owner.waitForStarted() && QTest::qWaitFor([&] { return QFileInfo::exists(ready); }, 5000),
                   "first swap recovery holds recovery-audit flock before cycle.lock work");
            const auto contender = runSwapWorker(root, data, direction, QProcessEnvironment::systemEnvironment());
            verify(contender.first == QProcess::NormalExit && contender.second == 51
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "parallel swap recovery cannot invert or bypass protocol lock ordering");
            verify(writeBarrier(release, QByteArrayLiteral("release"))
                       && owner.waitForFinished(10000) && owner.exitCode() == 0
                       && swapAtGoal(data, leftInode, rightInode)
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "sole swap lock owner reaches the durable exact goal");
            verify(runSwapWorker(root, data, direction, QProcessEnvironment::systemEnvironment()).second == 0,
                   "restart after parallel swap recovery is clean and idempotent");
        }
        for (const QByteArray &conflict : {QByteArrayLiteral("foreign-left"),
                                          QByteArrayLiteral("missing-left"),
                                          QByteArrayLiteral("missing-right"),
                                          QByteArrayLiteral("metadata-left")}) {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated occupied-name swap conflict fixture");
            const QString left = QDir(data).filePath(QStringLiteral("left"));
            const QString right = QDir(data).filePath(QStringLiteral("right"));
            QString protectedPath; QByteArray protectedPayload;
            if (conflict == QByteArrayLiteral("foreign-left")) {
                verify(QFile::rename(left, left + QStringLiteral(".original")),
                       "swap original is parked for foreign inode conflict");
                QFile foreign(left); protectedPath = left; protectedPayload = QByteArrayLiteral("FOREIGN");
                verify(foreign.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                           && foreign.write(protectedPayload) == protectedPayload.size(),
                       "foreign inode occupies one swap name");
            } else if (conflict == QByteArrayLiteral("missing-left")) {
                verify(QFile::rename(left, left + QStringLiteral(".missing")),
                       "left occupied swap name becomes missing");
            } else if (conflict == QByteArrayLiteral("missing-right")) {
                verify(QFile::rename(right, right + QStringLiteral(".missing")),
                       "right occupied swap name becomes missing");
            } else {
                verify(QFile::setPermissions(left, QFileDevice::ReadOwner),
                       "full swap snapshot metadata is changed");
            }
            const auto attempt = runSwapWorker(root, data, direction, QProcessEnvironment::systemEnvironment());
            verify(attempt.first == QProcess::NormalExit && attempt.second == 57
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "swap inode, missing-name, or metadata conflict fails closed with journal retained");
            if (!protectedPath.isEmpty()) {
                QFile protectedFile(protectedPath);
                verify(protectedFile.open(QIODevice::ReadOnly)
                           && protectedFile.readAll() == protectedPayload,
                       "detected swap conflict never overwrites the foreign occupant");
            }
        }
        for (const auto &race : {qMakePair(QStringLiteral("THISPC_RECOVERY_RACE_SWAP"),
                                           QByteArrayLiteral("FOREIGN")),
                                 qMakePair(QStringLiteral("THISPC_RECOVERY_RACE_AFTER_SWAP"),
                                           QByteArrayLiteral("AFTER-FOREIGN"))}) {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated non-cooperating swap TOCTOU fixture");
            QProcessEnvironment raced = QProcessEnvironment::systemEnvironment();
            raced.insert(race.first, QStringLiteral("1"));
            const auto attempt = runSwapWorker(root, data, direction, raced);
            verify(attempt.first == QProcess::NormalExit && attempt.second == 57
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "pre-syscall or post-syscall swap TOCTOU stops at uncertainty and retains journal");
            QByteArrayList payloads;
            const QDir dir(data);
            for (const QString &name : dir.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
                QFile file(dir.filePath(name));
                if (file.open(QIODevice::ReadOnly)) payloads.append(file.readAll());
            }
            verify(payloads.contains(QByteArrayLiteral("LEFT"))
                       && payloads.contains(QByteArrayLiteral("RIGHT"))
                       && payloads.contains(race.second),
                   "TOCTOU evidence preserves both originals and foreign payload without claiming fixed names");
        }
        {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated swap directory replacement fixture");
            const QString ready = testCase.filePath(QStringLiteral("directory-ready"));
            const QString release = testCase.filePath(QStringLiteral("directory-release"));
            QProcessEnvironment held = QProcessEnvironment::systemEnvironment();
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_AT"), QStringLiteral("before-intent"));
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_READY"), ready);
            held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_RELEASE"), release);
            QProcess process;
            process.setProcessEnvironment(held);
            process.start(QString::fromLocal8Bit(argv[0]), {QStringLiteral("batch_rename"),
                QStringLiteral("--stage4a1-swap-worker"), root, data,
                QString::fromLatin1(direction), QStringLiteral("recover")});
            verify(process.waitForStarted() && QTest::qWaitFor([&] { return QFileInfo::exists(ready); }, 5000),
                   "swap recovery pauses after opening and validating its stable data dirfd");
            const QString original = data + QStringLiteral(".original-directory");
            verify(QFile::rename(data, original) && QDir().mkdir(data)
                       && writeBarrier(release, QByteArrayLiteral("release"))
                       && process.waitForFinished(10000) && process.exitCode() == 57
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "non-cooperating directory substitution is detected after syscall and retains journal");
            verify(swapAtGoal(original, leftInode, rightInode),
                   "stable dirfd confines the exchange to the originally opened directory inode");
        }
    }
    const QList<QByteArray> swapRecoveryKillMarkers{
        "before-intent", "before-checkpoint", "before-journal-file-fsync",
        "after-journal-file-fsync", "before-journal-dir-fsync", "after-journal-dir-fsync",
        "after-checkpoint", "after-intent", "before-syscall", "after-syscall",
        "before-data-dir-fsync", "after-data-dir-fsync", "after-postcheck",
        "after-verified", "after-goal-before-unlink"};
    for (const QByteArray &direction : {QByteArrayLiteral("forward"), QByteArrayLiteral("undo"),
                                        QByteArrayLiteral("redo")}) {
        for (const QByteArray &marker : swapRecoveryKillMarkers) {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated second-SIGKILL swap recovery fixture");
            QProcessEnvironment killed = QProcessEnvironment::systemEnvironment();
            killed.insert(QStringLiteral("XDG_DATA_HOME"), testCase.filePath(QStringLiteral("xdg")));
            killed.insert(QStringLiteral("THISPC_RECOVERY_RECOVER_KILL_AT"), QString::fromLatin1(marker));
            const auto first = runSwapWorker(root, data, direction, killed);
            verify(first.first == QProcess::CrashExit,
                   "swap recovery itself is SIGKILLed at its deterministic marker");
            QProcessEnvironment clean = QProcessEnvironment::systemEnvironment();
            clean.insert(QStringLiteral("XDG_DATA_HOME"), testCase.filePath(QStringLiteral("xdg")));
            const auto second = runSwapWorker(root, data, direction, clean);
            const auto third = runSwapWorker(root, data, direction, clean);
            verify(second.first == QProcess::NormalExit && second.second == 0
                       && third.first == QProcess::NormalExit && third.second == 0
                       && swapAtGoal(data, leftInode, rightInode)
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "successive swap restarts reach exact inode/payload goal without a second exchange");
        }
        for (const QByteArray &fault : {QByteArrayLiteral("journal-file-fsync"),
                                        QByteArrayLiteral("journal-dir-fsync"),
                                        QByteArrayLiteral("data-dir-fsync"),
                                        QByteArrayLiteral("cleanup-dir-fsync")}) {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated swap fsync failure fixture");
            QProcessEnvironment failed = QProcessEnvironment::systemEnvironment();
            failed.insert(QStringLiteral("THISPC_RECOVERY_FAIL_AT"), QString::fromLatin1(fault));
            const auto attempt = runSwapWorker(root, data, direction, failed);
            verify(attempt.first == QProcess::NormalExit && attempt.second == 57
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "swap fsync failure retains the journal and fails closed");
            verify(runSwapWorker(root, data, direction, QProcessEnvironment::systemEnvironment()).second == 0
                       && swapAtGoal(data, leftInode, rightInode),
                   "swap recovery retries safely after an injected fsync failure");
        }
        for (const QByteArray &fault : {QByteArrayLiteral("ENOSYS"), QByteArrayLiteral("EOPNOTSUPP")}) {
            QTemporaryDir testCase; QString root, data; quint64 leftInode = 0, rightInode = 0;
            verify(testCase.isValid() && makeSwapCrashFixture(testCase, direction, &root, &data,
                                                               &leftInode, &rightInode),
                   "isolated unsupported swap exchange fixture");
            QProcessEnvironment failed = QProcessEnvironment::systemEnvironment();
            failed.insert(QStringLiteral("THISPC_RECOVERY_RENAMEAT2_ERRNO"), QString::fromLatin1(fault));
            verify(runSwapWorker(root, data, direction, failed).second == 57
                       && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "unsupported swap exchange fails closed and retains its journal");
            QFile left(QDir(data).filePath(QStringLiteral("left")));
            QFile right(QDir(data).filePath(QStringLiteral("right")));
            verify(left.open(QIODevice::ReadOnly) && right.open(QIODevice::ReadOnly)
                       && left.readAll() == QByteArrayLiteral("LEFT")
                       && right.readAll() == QByteArrayLiteral("RIGHT"),
                   "unsupported swap exchange changes neither occupied name");
        }
    }
    for (const QByteArray &marker : v2KillMarkers) {
        QTemporaryDir crashCase;
        verify(crashCase.isValid(), "isolated v2 SIGKILL case");
        const QString data = crashCase.filePath(QStringLiteral("data"));
        const QString root = crashCase.filePath(QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
        verify(QDir().mkpath(data) && QDir().mkpath(root), "isolated v2 directories");
        verify(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                             | QFileDevice::ExeOwner),
               "isolated recovery root is private");
        QList<QUrl> sources;
        for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                    QStringLiteral("2"), QStringLiteral("42")}) {
            QFile file(QDir(data).filePath(name));
            verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && file.write(name.toUtf8()) == name.toUtf8().size(),
                   "v2 crash fixture payload");
            file.close();
            sources.append(QUrl::fromLocalFile(file.fileName()));
        }
        const auto plan = makeBatchRenamePlan(sources, QStringLiteral("4"), {}, false, 1, 2);
        QProcess worker;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("THISPC_RECOVERY_KILL_AT"),
                           QString::fromLatin1(marker));
        worker.setProcessEnvironment(environment);
        worker.start(QString::fromLocal8Bit(argv[0]),
                     {QStringLiteral("batch_rename"), QStringLiteral("--stage2-v2-kill-worker"),
                      root, data});
        verify(worker.waitForStarted() && worker.waitForFinished(10000)
                   && worker.exitStatus() == QProcess::CrashExit,
               "dedicated v2 helper is killed at deterministic marker");
        BatchRenameRecoveryGate recovery(root);
        if (marker == QByteArrayLiteral("before-prepared")) {
            verify(!recovery.mutationsBlocked()
                       && batchRenameLinearMappingMatches(plan, false, 0)
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "kill before prepared changes nothing and safely clears its stale lock");
        } else {
            const bool recovered = !recovery.mutationsBlocked()
                && batchRenameLinearMappingMatches(plan, true, 0)
                && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty();
            if (!recovered)
                qFatal("FAIL v2 recovery marker %s: %s", marker.constData(),
                       qPrintable(recovery.message()));
            ++checks;
        }
    }

    for (const QByteArray &direction : {QByteArrayLiteral("undo"), QByteArrayLiteral("redo")}) {
        const bool undo = direction == QByteArrayLiteral("undo");
        for (const QByteArray &marker : v2KillMarkers) {
            QTemporaryDir crashCase;
            verify(crashCase.isValid(), "isolated v2 Undo/Redo SIGKILL case");
            const QString data = crashCase.filePath(QStringLiteral("data"));
            const QString root = crashCase.filePath(
                QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
            verify(QDir().mkpath(data) && QDir().mkpath(root),
                   "isolated v2 Undo/Redo directories");
            verify(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ExeOwner),
                   "isolated v2 Undo/Redo recovery root is private");
            QList<QUrl> sources;
            for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                        QStringLiteral("2"), QStringLiteral("42")}) {
                QFile file(QDir(data).filePath(name));
                verify(file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                           && file.write(name.toUtf8()) == name.toUtf8().size(),
                       "v2 Undo/Redo crash fixture payload");
                file.close();
                sources.append(QUrl::fromLocalFile(file.fileName()));
            }
            const auto plan = makeBatchRenamePlan(sources, QStringLiteral("4"), {}, false, 1, 2);
            QProcess worker;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("XDG_DATA_HOME"),
                               crashCase.filePath(QStringLiteral("xdg")));
            environment.insert(QStringLiteral("THISPC_RECOVERY_KILL_AT"),
                               QString::fromLatin1(marker));
            worker.setProcessEnvironment(environment);
            worker.start(QString::fromLocal8Bit(argv[0]),
                         {QStringLiteral("batch_rename"),
                          QStringLiteral("--stage3-v2-history-kill-worker"), root, data,
                          QString::fromLatin1(direction)});
            if (!worker.waitForStarted() || !worker.waitForFinished(10000)
                || worker.exitStatus() != QProcess::CrashExit)
                qFatal("FAIL v2 Undo/Redo helper marker %s: status=%d code=%d stderr=%s",
                       marker.constData(), int(worker.exitStatus()), worker.exitCode(),
                       worker.readAllStandardError().constData());
            ++checks;
            BatchRenameRecoveryGate recovery(root);
            const bool beforeJournal = marker == QByteArrayLiteral("before-prepared");
            const bool expectedMapping = beforeJournal
                ? batchRenameLinearMappingMatches(plan, undo, 0)
                : batchRenameLinearMappingMatches(plan, !undo, 0);
            verify(!recovery.mutationsBlocked() && expectedMapping
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "startup finishes the recorded v2 Undo/Redo direction idempotently");
            recovery.refresh();
            verify(!recovery.mutationsBlocked(),
                   "second restart after v2 Undo/Redo recovery is idempotent");
        }
    }

    const auto makeCrashFixture = [&](QTemporaryDir &testCase, bool undo, QString *rootOut,
                                      QString *dataOut, BatchRenamePlan *planOut) {
        const QString data = testCase.filePath(QStringLiteral("data"));
        const QString root = testCase.filePath(QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
        if (!QDir().mkpath(data) || !QDir().mkpath(root)
            || !QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner)) return false;
        QList<QUrl> sources;
        for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                    QStringLiteral("2"), QStringLiteral("42")}) {
            QFile file(QDir(data).filePath(name));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                || file.write(name.toUtf8()) != name.toUtf8().size()) return false;
            file.close();
            sources.append(QUrl::fromLocalFile(file.fileName()));
        }
        const auto plan = makeBatchRenamePlan(sources, QStringLiteral("4"), {}, false, 1, 2);
        QProcess crash;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_DATA_HOME"),
                           testCase.filePath(QStringLiteral("xdg")));
        environment.insert(QStringLiteral("THISPC_RECOVERY_KILL_AT"), QStringLiteral("after-prepared"));
        crash.setProcessEnvironment(environment);
        crash.start(QString::fromLocal8Bit(argv[0]),
                    {QStringLiteral("batch_rename"),
                     QStringLiteral("--stage3-v2-history-kill-worker"), root, data,
                     undo ? QStringLiteral("undo") : QStringLiteral("redo")});
        if (!plan.isValid() || !crash.waitForStarted() || !crash.waitForFinished(10000)
            || crash.exitStatus() != QProcess::CrashExit
            || BatchRenameRecoveryJournal::pendingJournal(root).isEmpty()) return false;
        *rootOut = root; *dataOut = data; *planOut = plan;
        return true;
    };

    const QString blackBoxView = QString::fromLocal8Bit(qgetenv("THISPC_BLACKBOX_VIEW"));
    const QString productionView = QString::fromLocal8Bit(qgetenv("THISPC_PRODUCTION_VIEW"));
    verify(QFileInfo(blackBoxView).isExecutable() && QFileInfo(productionView).isExecutable(),
           "built production and Stage 3C.1 black-box application binaries exist");
    const auto readBytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto verifyGoalIdentity = [&](const BatchRenamePlan &plan, bool undo) {
        for (const auto &entry : plan.entries) {
            const QString path = (undo ? entry.source : entry.destination).toLocalFile();
            struct stat identity {};
            if (::lstat(QFile::encodeName(path).constData(), &identity) != 0
                || quint64(identity.st_dev) != entry.sourceDevice
                || quint64(identity.st_ino) != entry.sourceInode
                || readBytes(path) != entry.oldName.toUtf8()) return false;
        }
        return true;
    };
    const auto blackBoxEnvironment = [&](QTemporaryDir &testCase, const QString &root,
                                         const QString &trace, const QString &heartbeat) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const QString xdg = testCase.filePath(QStringLiteral("xdg"));
        const QString runtime = testCase.filePath(QStringLiteral("runtime"));
        QDir().mkpath(runtime);
        QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), xdg);
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), testCase.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), testCase.filePath(QStringLiteral("cache")));
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        environment.insert(QStringLiteral("THISPC_RECOVERY_ROOT"), root);
        environment.insert(QStringLiteral("THISPC_RECOVERY_UI_TRACE"), trace);
        environment.insert(QStringLiteral("THISPC_RECOVERY_UI_HEARTBEAT"), heartbeat);
        environment.insert(QStringLiteral("THISPC_RECOVERY_DIRECTION_DELAY_MS"), QStringLiteral("1200"));
        environment.insert(QStringLiteral("THISPC_RECOVERY_ACTIVE_DELAY_MS"), QStringLiteral("1200"));
        environment.insert(QStringLiteral("THISPC_RECOVERY_TEST_EXIT"), QStringLiteral("1"));
        return environment;
    };
    for (const QByteArray &direction : {QByteArrayLiteral("undo"), QByteArrayLiteral("redo")}) {
        const bool undo = direction == QByteArrayLiteral("undo");
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "black-box Undo/Redo startup fixture is private and isolated");
        const QString trace = testCase.filePath(QStringLiteral("ui-trace"));
        const QString heartbeat = testCase.filePath(QStringLiteral("ui-heartbeat"));
        QProcess application;
        application.setProcessEnvironment(blackBoxEnvironment(testCase, root, trace, heartbeat));
        application.start(blackBoxView, {data});
        verify(application.waitForStarted(5000) && application.waitForFinished(15000)
                   && application.exitStatus() == QProcess::NormalExit && application.exitCode() == 0,
               "real built test-mode thispc-view completes startup recovery and exits cleanly");
        const QByteArray ui = readBytes(trace);
        const QByteArray label = undo ? QByteArrayLiteral("Undo") : QByteArrayLiteral("Redo");
        verify(ui.contains("Checking interrupted Batch Rename " + label + " recovery")
                   && ui.contains("Finishing interrupted Batch Rename " + label + " recovery")
                   && ui.contains("Interrupted Batch Rename " + label + " recovery completed")
                   && ui.contains("history from before restart is not restored"),
               "persistent nonmodal UI reports directional checking, recovery, success, and lost history");
        verify(readBytes(heartbeat).count("tick\n") >= 20,
               "GUI event loop remains responsive while test recovery worker is slowed");
        verify(BatchRenameRecoveryJournal::pendingJournal(root).isEmpty()
                   && batchRenameLinearMappingMatches(plan, !undo, 0)
                   && verifyGoalIdentity(plan, undo),
               "black-box recovery reaches exact names, payloads, devices, and inodes");

        const QString secondTrace = testCase.filePath(QStringLiteral("ui-trace-second"));
        const QString secondHeartbeat = testCase.filePath(QStringLiteral("ui-heartbeat-second"));
        QProcess second;
        second.setProcessEnvironment(blackBoxEnvironment(testCase, root, secondTrace, secondHeartbeat));
        second.start(blackBoxView, {data});
        verify(second.waitForStarted(5000) && second.waitForFinished(10000)
                   && second.exitCode() == 0
                   && readBytes(secondTrace).contains(
                       "Undo/Redo history from before restart is not restored")
                   && verifyGoalIdentity(plan, undo),
               "second real application restart is idempotent and reports absent old history");
    }
    {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, true, &root, &data, &plan),
               "black-box conflict fixture is isolated");
        const auto &entry = plan.entries.at(plan.executionOrder.last());
        const QString guarded = entry.destination.toLocalFile();
        const QString original = guarded + QStringLiteral(".original");
        verify(QFile::rename(guarded, original), "guarded recovery source inode is moved aside");
        QFile outsider(guarded);
        verify(outsider.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && outsider.write("FOREIGN-BLACKBOX") == 16,
               "foreign conflict inode is created without deleting the original");
        outsider.close();
        struct stat before {};
        verify(::lstat(QFile::encodeName(guarded).constData(), &before) == 0,
               "foreign conflict identity is captured");
        const QString trace = testCase.filePath(QStringLiteral("conflict-trace"));
        const QString heartbeat = testCase.filePath(QStringLiteral("conflict-heartbeat"));
        QProcess application;
        auto environment = blackBoxEnvironment(testCase, root, trace, heartbeat);
        environment.remove(QStringLiteral("THISPC_RECOVERY_DIRECTION_DELAY_MS"));
        environment.remove(QStringLiteral("THISPC_RECOVERY_ACTIVE_DELAY_MS"));
        application.setProcessEnvironment(environment);
        application.start(blackBoxView, {data});
        verify(application.waitForStarted(5000) && application.waitForFinished(10000)
                   && application.exitCode() == 0,
               "real application conflict startup exits safely");
        struct stat after {};
        verify(::lstat(QFile::encodeName(guarded).constData(), &after) == 0
                   && quint64(after.st_dev) == quint64(before.st_dev)
                   && quint64(after.st_ino) == quint64(before.st_ino)
                   && readBytes(guarded) == QByteArrayLiteral("FOREIGN-BLACKBOX")
                   && QFileInfo::exists(original)
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty()
                   && readBytes(trace).contains("Batch Rename Undo recovery has a conflict")
                   && readBytes(trace).contains("journal is preserved"),
               "conflict status persists, mutation stays blocked, journal and foreign inode are preserved");
    }
    {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, false, &root, &data, &plan),
               "production Redo recovery fixture is isolated");
        verify(verifyGoalIdentity(plan, true),
               "production fixture starts at the exact pre-Redo names and inode identities");
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        const QString runtime = testCase.filePath(QStringLiteral("production-runtime"));
        QDir().mkpath(runtime);
        QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), testCase.filePath(QStringLiteral("xdg")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), testCase.filePath(QStringLiteral("production-config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), testCase.filePath(QStringLiteral("production-cache")));
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        QProcess production;
        production.setProcessEnvironment(environment);
        production.start(productionView, {data});
        verify(production.waitForStarted(5000), "real production thispc-view starts");
        verify(QTest::qWaitFor([&] {
                   return BatchRenameRecoveryJournal::pendingJournal(root).isEmpty()
                       && verifyGoalIdentity(plan, false);
               }, 10000),
               "normal production startup finishes the exact recorded v2 Redo direction");
        production.terminate();
        if (!production.waitForFinished(5000)) { production.kill(); production.waitForFinished(5000); }
    }
    {
        QTemporaryDir clean;
        const QString root = clean.filePath(QStringLiteral("xdg/thispc-view/batch-rename-recovery"));
        verify(clean.isValid() && QDir().mkpath(root)
                   && QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                   | QFileDevice::ExeOwner),
               "clean black-box startup root is isolated and private");
        const QString trace = clean.filePath(QStringLiteral("clean-trace"));
        const QString heartbeat = clean.filePath(QStringLiteral("clean-heartbeat"));
        QProcess cleanStart;
        cleanStart.setProcessEnvironment(blackBoxEnvironment(clean, root, trace, heartbeat));
        cleanStart.start(blackBoxView, {clean.path()});
        verify(cleanStart.waitForStarted(5000) && cleanStart.waitForFinished(10000)
                   && cleanStart.exitCode() == 0
                   && readBytes(trace).contains("Crash recovery check completed")
                   && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "clean real-application startup remains crash-free and unblocked");
    }
    const auto runRecovery = [&](const QString &root, const QProcessEnvironment &environment) {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.start(QString::fromLocal8Bit(argv[0]),
                      {QStringLiteral("batch_rename"), QStringLiteral("--stage2-recovery-worker"), root});
        verify(process.waitForStarted() && process.waitForFinished(10000),
               "dedicated recovery helper completes or crashes deterministically");
        return qMakePair(process.exitStatus(), process.exitCode());
    };
    const QList<QByteArray> recoveryKillMarkers{
        "before-intent", "before-checkpoint", "before-journal-file-fsync",
        "after-journal-file-fsync", "before-journal-dir-fsync", "after-journal-dir-fsync",
        "after-checkpoint", "after-intent", "before-syscall", "after-syscall",
        "before-data-dir-fsync", "after-data-dir-fsync", "after-verified",
        "after-goal-before-unlink"};
    for (const QByteArray &direction : {QByteArrayLiteral("undo"), QByteArrayLiteral("redo")}) {
        const bool undo = direction == QByteArrayLiteral("undo");
        for (const QByteArray &marker : recoveryKillMarkers) {
            QTemporaryDir testCase;
            QString root, data; BatchRenamePlan plan;
            verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
                   "isolated repeated-crash Undo/Redo recovery fixture");
            QProcessEnvironment killed = QProcessEnvironment::systemEnvironment();
            killed.insert(QStringLiteral("THISPC_RECOVERY_RECOVER_KILL_AT"), QString::fromLatin1(marker));
            const auto first = runRecovery(root, killed);
            verify(first.first == QProcess::CrashExit,
                   "Undo/Redo recovery itself is SIGKILLed at its deterministic marker");
            const auto second = runRecovery(root, QProcessEnvironment::systemEnvironment());
            const auto third = runRecovery(root, QProcessEnvironment::systemEnvironment());
            verify(second.first == QProcess::NormalExit && second.second == 0
                       && third.first == QProcess::NormalExit && third.second == 0
                       && batchRenameLinearMappingMatches(plan, !undo, 0)
                       && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
                   "successive restarts finish recorded Undo/Redo idempotently");
        }
    }

    for (const QByteArray &direction : {QByteArrayLiteral("undo"), QByteArrayLiteral("redo")}) {
    const bool undo = direction == QByteArrayLiteral("undo");
    for (const QByteArray &fault : {QByteArrayLiteral("journal-file-fsync"),
                                    QByteArrayLiteral("journal-dir-fsync"),
                                    QByteArrayLiteral("data-dir-fsync")}) {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "isolated fsync failure fixture");
        QProcessEnvironment failed = QProcessEnvironment::systemEnvironment();
        failed.insert(QStringLiteral("THISPC_RECOVERY_FAIL_AT"), QString::fromLatin1(fault));
        const auto first = runRecovery(root, failed);
        verify(first.first == QProcess::NormalExit && first.second == 33
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "fsync failure fails closed and retains the recovery journal");
        const auto retry = runRecovery(root, QProcessEnvironment::systemEnvironment());
        verify(retry.second == 0 && batchRenameLinearMappingMatches(plan, !undo, 0),
               "clean restart after fsync failure reaches the exact goal");
    }
    for (const QByteArray &fault : {QByteArrayLiteral("ENOSYS"), QByteArrayLiteral("EOPNOTSUPP")}) {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "isolated unsupported renameat2 fixture");
        QProcessEnvironment failed = QProcessEnvironment::systemEnvironment();
        failed.insert(QStringLiteral("THISPC_RECOVERY_RENAMEAT2_ERRNO"), QString::fromLatin1(fault));
        const auto first = runRecovery(root, failed);
        verify(first.second == 33 && batchRenameLinearMappingMatches(plan, undo, 0)
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "unsupported renameat2 fails closed without changing any source");
        verify(runRecovery(root, QProcessEnvironment::systemEnvironment()).second == 0,
               "recovery can be retried after renameat2 support returns");
    }
    {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "isolated precheck-to-syscall race fixture");
        QProcessEnvironment raced = QProcessEnvironment::systemEnvironment();
        raced.insert(QStringLiteral("THISPC_RECOVERY_RACE_OCCUPY"), QStringLiteral("1"));
        verify(runRecovery(root, raced).second == 33
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "deterministic destination race is rejected without overwrite");
        bool foreignPreserved = false;
        for (const QString &name : QDir(data).entryList(QDir::Files | QDir::NoDotAndDotDot)) {
            QFile candidate(QDir(data).filePath(name));
            if (candidate.open(QIODevice::ReadOnly)
                && candidate.readAll() == QByteArrayLiteral("FOREIGN")) foreignPreserved = true;
        }
        bool originalsPreserved = true;
        for (int row : plan.executionOrder) {
            const auto &entry = plan.entries.at(row);
            if (!batchRenameSnapshotAt(entry, undo ? entry.destination : entry.source))
                originalsPreserved = false;
        }
        verify(foreignPreserved && originalsPreserved,
               "foreign race inode and every original source remain untouched");
    }
    {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "isolated parallel recovery fixture");
        const QString ready = testCase.filePath(QStringLiteral("recovery-a-ready"));
        const QString release = testCase.filePath(QStringLiteral("recovery-a-release"));
        QProcessEnvironment held = QProcessEnvironment::systemEnvironment();
        held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_AT"), QStringLiteral("before-intent"));
        held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_READY"), ready);
        held.insert(QStringLiteral("THISPC_RECOVERY_HOLD_RELEASE"), release);
        QProcess first;
        first.setProcessEnvironment(held);
        first.start(QString::fromLocal8Bit(argv[0]),
                    {QStringLiteral("batch_rename"), QStringLiteral("--stage2-recovery-worker"), root});
        verify(first.waitForStarted() && QTest::qWaitFor([&] { return QFileInfo::exists(ready); }, 5000),
               "first recovery holds protocol lock before its first intent");
        const auto contender = runRecovery(root, QProcessEnvironment::systemEnvironment());
        verify(contender.first == QProcess::NormalExit && contender.second == 33,
               "second concurrent recovery fails closed behind the protocol lock");
        verify(writeBarrier(release, QByteArrayLiteral("release"))
                   && first.waitForFinished(10000) && first.exitCode() == 0
                   && batchRenameLinearMappingMatches(plan, !undo, 0)
                   && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "lock owner alone finishes recovery and durable cleanup");
        verify(runRecovery(root, QProcessEnvironment::systemEnvironment()).second == 0,
               "post-race restart is idempotently clean");
    }
    for (const QByteArray &conflict : {QByteArrayLiteral("foreign-source"),
                                      QByteArrayLiteral("missing-source"),
                                      QByteArrayLiteral("metadata-change"),
                                      QByteArrayLiteral("occupied-hole"),
                                      QByteArrayLiteral("replaced-directory")}) {
        QTemporaryDir testCase; QString root, data; BatchRenamePlan plan;
        verify(testCase.isValid() && makeCrashFixture(testCase, undo, &root, &data, &plan),
               "isolated recovery conflict fixture");
        const int firstRecoveryRow = undo ? plan.executionOrder.last()
                                          : plan.executionOrder.first();
        const auto &firstEntry = plan.entries.at(firstRecoveryRow);
        const QString source = (undo ? firstEntry.destination : firstEntry.source).toLocalFile();
        const QString destination = (undo ? firstEntry.source : firstEntry.destination).toLocalFile();
        QString protectedPath;
        QByteArray protectedPayload;
        if (conflict == QByteArrayLiteral("foreign-source")) {
            protectedPath = source;
            verify(QFile::rename(source, source + QStringLiteral(".original")),
                   "original source is moved aside for foreign inode conflict");
            QFile outsider(source); protectedPayload = QByteArrayLiteral("FOREIGN-SOURCE");
            verify(outsider.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && outsider.write(protectedPayload) == protectedPayload.size(),
                   "foreign source inode is created");
        } else if (conflict == QByteArrayLiteral("missing-source")) {
            verify(QFile::rename(source, source + QStringLiteral(".missing")),
                   "source is made missing without deleting its inode");
        } else if (conflict == QByteArrayLiteral("metadata-change")) {
            verify(QFile::setPermissions(source, QFileDevice::ReadOwner),
                   "source metadata is deterministically changed");
        } else if (conflict == QByteArrayLiteral("occupied-hole")) {
            protectedPath = destination; protectedPayload = QByteArrayLiteral("FOREIGN-HOLE");
            QFile outsider(destination);
            verify(outsider.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && outsider.write(protectedPayload) == protectedPayload.size(),
                   "expected destination hole is occupied by a foreign inode");
        } else {
            verify(QFile::rename(data, data + QStringLiteral(".original-directory"))
                       && QDir().mkdir(data),
                   "recorded data directory is replaced with a different inode");
        }
        const auto attempt = runRecovery(root, QProcessEnvironment::systemEnvironment());
        verify(attempt.first == QProcess::NormalExit && attempt.second == 33
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "source, hole, metadata, or directory conflict fails closed and retains journal");
        if (!protectedPath.isEmpty()) {
            QFile protectedFile(protectedPath);
            verify(protectedFile.open(QIODevice::ReadOnly)
                       && protectedFile.readAll() == protectedPayload,
                   "recovery never overwrites the foreign conflict inode");
        }
    }
    }

    const auto writeJson = [&](const QString &path, const QJsonObject &object) {
        QFile file(path);
        const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly)
            && file.write(bytes) == bytes.size() && file.flush();
    };
    const auto makeV2 = [&](const QString &directory, const QString &source,
                            const QString &destination, int completed = 0) {
        struct stat identity {};
        struct stat directoryIdentity {};
        verify(::lstat(QFile::encodeName(source).constData(), &identity) == 0,
               "v2 fixture source identity is readable");
        verify(::lstat(QFile::encodeName(directory).constData(), &directoryIdentity) == 0,
               "v2 fixture directory identity is readable");
        QJsonObject item{{QStringLiteral("row"), 0}, {QStringLiteral("source"), source},
            {QStringLiteral("destination"), destination},
            {QStringLiteral("device"), QString::number(quint64(identity.st_dev))},
            {QStringLiteral("inode"), QString::number(quint64(identity.st_ino))},
            {QStringLiteral("mode"), QString::number(quint64(identity.st_mode))},
            {QStringLiteral("size"), QString::number(qint64(identity.st_size))},
            {QStringLiteral("mtimeNs"), QString::number(
                qint64(identity.st_mtim.tv_sec) * 1000000000LL + identity.st_mtim.tv_nsec)},
            {QStringLiteral("type"), S_ISLNK(identity.st_mode) ? QStringLiteral("symlink")
                : (S_ISDIR(identity.st_mode) ? QStringLiteral("directory") : QStringLiteral("file"))}};
        QJsonObject object{{QStringLiteral("schema"), 2},
            {QStringLiteral("scope"), QStringLiteral("recovery-audit-v2")},
            {QStringLiteral("kind"), QStringLiteral("linear")},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("direction"), QStringLiteral("forward")},
            {QStringLiteral("phase"), QStringLiteral("prepared")},
            {QStringLiteral("completedSteps"), completed},
            {QStringLiteral("directoryDevice"), QString::number(quint64(directoryIdentity.st_dev))},
            {QStringLiteral("directoryInode"), QString::number(quint64(directoryIdentity.st_ino))},
            {QStringLiteral("items"), QJsonArray{item}}};
        object.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(object));
        return object;
    };
    {
        QTemporaryDir startupFiles;
        verify(startupFiles.isValid(), "eligible startup recovery data is isolated");
        const QString firstSource = startupFiles.filePath(QStringLiteral("alpha.txt"));
        const QString secondSource = startupFiles.filePath(QStringLiteral("beta.txt"));
        for (const QString &path : {firstSource, secondSource}) {
            QFile sourceFile(path);
            verify(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && sourceFile.write(QFileInfo(path).fileName().toUtf8()) > 0,
                   "eligible startup recovery payload is created");
        }
        const QString firstDestination = startupFiles.filePath(QStringLiteral("startup-v2-alpha.txt"));
        const QString secondDestination = startupFiles.filePath(QStringLiteral("startup-v2-beta.txt"));
        QJsonObject pendingV2 = makeV2(startupFiles.path(), firstSource, firstDestination);
        const QJsonObject second = makeV2(startupFiles.path(), secondSource, secondDestination)
                                       .value(QStringLiteral("items")).toArray().first().toObject();
        QJsonArray items = pendingV2.value(QStringLiteral("items")).toArray();
        QJsonObject secondRow = second;
        secondRow.insert(QStringLiteral("row"), 1);
        items.append(secondRow);
        pendingV2.insert(QStringLiteral("items"), items);
        pendingV2.insert(QStringLiteral("digest"), BatchRenameRecoveryDetail::digest(pendingV2));
        const QString journal = processRecoveryRoot.filePath(QStringLiteral("linear-startup-v2.json"));
        verify(writeJson(journal, pendingV2), "eligible startup v2 fixture is isolated");
        auto &startupGate = BatchRenameRecoveryGate::instance();
        startupGate.beginStartupScan();
        QFutureWatcher<void> startupScan;
        startupScan.setFuture(QtConcurrent::run([&startupGate] { startupGate.completeStartupScan(); }));
        verify(QTest::qWaitFor([&] { return startupScan.isFinished(); }, 5000),
               "production-style startup scan completes asynchronously");
        QFile firstResult(firstDestination);
        QFile secondResult(secondDestination);
        verify(!startupGate.mutationsBlocked()
                   && !QFileInfo::exists(journal)
                   && !QFileInfo::exists(firstSource)
                   && !QFileInfo::exists(secondSource)
                   && firstResult.open(QIODevice::ReadOnly)
                   && firstResult.readAll() == QByteArrayLiteral("alpha.txt")
                   && secondResult.open(QIODevice::ReadOnly)
                   && secondResult.readAll() == QByteArrayLiteral("beta.txt"),
               "production-style startup safely finishes exactly one eligible forward v2 Execute");
        startupGate.refresh();
        verify(!startupGate.mutationsBlocked(), "clean restart after startup recovery is idempotent");
    }
    {
        QTemporaryDir auditRoot;
        verify(auditRoot.isValid(), "isolated read-only recovery audit root");
        BatchRenameRecoveryGate empty(auditRoot.path());
        verify(!empty.mutationsBlocked() && empty.audit().entries.isEmpty(),
               "zero journals leaves mutation fence open");
        const QString source = alpha.toLocalFile();
        const QString destination = files.filePath(QStringLiteral("audit-destination.txt"));
        const QString validPath = auditRoot.filePath(QStringLiteral("linear-valid.json"));
        verify(writeJson(validPath, makeV2(files.path(), source, destination)),
               "valid v2 fixture is written only in disposable root");
        QFile before(validPath); verify(before.open(QIODevice::ReadOnly), "v2 fixture is readable");
        const QByteArray originalBytes = before.readAll(); before.close();
        const auto valid = parseBatchRenameRecoveryJournal(validPath);
        verify(valid.kind == BatchRenameRecoveryKind::V2
                   && valid.mapping == BatchRenameRecoveryMapping::Initial,
               "strict v2 digest and complete inode/hole mapping validate read-only");
        QFile after(validPath); verify(after.open(QIODevice::ReadOnly)
                   && after.readAll() == originalBytes,
               "v2 inspection does not rewrite its journal");
        empty.refresh();
        verify(empty.mutationsBlocked() && empty.audit().entries.size() == 1,
               "one journal closes the central recovery fence");
        QFile occupied(destination); verify(occupied.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "foreign hole occupant fixture is created"); occupied.write("foreign"); occupied.close();
        verify(parseBatchRenameRecoveryJournal(validPath).mapping
                   == BatchRenameRecoveryMapping::ForeignOrOccupied,
               "foreign occupant in an expected hole fails closed");
        QJsonObject corrupt = makeV2(files.path(), source, destination);
        corrupt.insert(QStringLiteral("digest"), QStringLiteral("00"));
        verify(writeJson(auditRoot.filePath(QStringLiteral("cycle-corrupt.json")), corrupt),
               "corrupt digest fixture is isolated");
        empty.refresh();
        verify(empty.audit().entries.size() == 2 && empty.mutationsBlocked(),
               "multiple journals enumerate completely and fail closed");
    }
    {
        QTemporaryDir variants;
        verify(variants.isValid(), "isolated schema variants root");
        verify(writeJson(variants.filePath(QStringLiteral("cycle-v1.json")),
                         QJsonObject{{QStringLiteral("schema"), 1}}), "v1 fixture");
        verify(writeJson(variants.filePath(QStringLiteral("linear-unknown.json")),
                         QJsonObject{{QStringLiteral("schema"), 99}}), "unknown fixture");
        QFile truncated(variants.filePath(QStringLiteral("cycle-truncated.json")));
        verify(truncated.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && truncated.write("{\"schema\":2") > 0, "truncated fixture"); truncated.close();
        QFile suspicious(variants.filePath(QStringLiteral(".journal.tmp")));
        verify(suspicious.open(QIODevice::WriteOnly | QIODevice::NewOnly), "temporary artifact fixture");
        suspicious.write("temporary"); suspicious.close();
        BatchRenameRecoveryGate gate(variants.path());
        int v1 = 0, unknown = 0, corrupt = 0, temporary = 0;
        for (const auto &entry : gate.audit().entries) {
            v1 += entry.kind == BatchRenameRecoveryKind::V1Manual;
            unknown += entry.kind == BatchRenameRecoveryKind::Unknown;
            corrupt += entry.kind == BatchRenameRecoveryKind::Corrupt;
            temporary += entry.kind == BatchRenameRecoveryKind::SuspiciousTemporary;
        }
        verify(v1 == 1 && unknown == 1 && corrupt == 1 && temporary == 1,
               "v1/manual-only, unknown, corrupt and suspicious temp enumerate distinctly");
    }
    {
        QTemporaryDir locks;
        verify(locks.isValid(), "isolated lock audit root");
        BatchRenameRecoveryGate holder(locks.path());
        BatchRenameRecoveryGate contender(locks.path());
        verify(holder.audit().inspectorLockHeld
                   && contender.audit().writerLock == BatchRenameRecoveryLock::Live
                   && contender.mutationsBlocked(),
               "kernel-held inspector lock prevents a second auditing process");
    }
    {
        QTemporaryDir race;
        verify(race.isValid(), "isolated multiprocess bootstrap fixture");
        const QString xdg = race.filePath(QStringLiteral("xdg-data"));
        verify(QDir().mkdir(xdg), "isolated XDG_DATA_HOME is created");
        const QString root = xdg + QStringLiteral("/thispc-view/batch-rename-recovery");
        verify(!QFileInfo::exists(root), "bootstrap recovery root starts absent");
        const QString first = race.filePath(QStringLiteral("one"));
        const QString second = race.filePath(QStringLiteral("two"));
        for (const QString &path : {first, second}) {
            QFile fixture(path);
            verify(fixture.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                       && fixture.write(QFileInfo(path).fileName().toUtf8()) > 0,
                   "multiprocess journal source fixture is created");
        }
        auto startWorker = [&](const QStringList &arguments) {
            auto process = std::make_unique<QProcess>();
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("XDG_DATA_HOME"), xdg);
            process->setProcessEnvironment(environment);
            process->start(QCoreApplication::applicationFilePath(),
                           QStringList{QStringLiteral("batch_rename")} + arguments);
            verify(process->waitForStarted(5000), "multiprocess worker starts");
            return process;
        };
        auto readState = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };

        const QString auditReady = race.filePath(QStringLiteral("audit-a-ready"));
        const QString auditRelease = race.filePath(QStringLiteral("audit-a-release"));
        const QString auditEffect = race.filePath(QStringLiteral("audit-a-effect"));
        auto audit = startWorker({QStringLiteral("--stage1c-audit-worker"), root,
                                  auditReady, auditRelease, auditEffect, QStringLiteral("unused")});
        verify(QTest::qWaitFor([&] { return QFileInfo::exists(auditReady); }, 5000)
                   && readState(auditReady) == QByteArrayLiteral("clean"),
               "A completes final clean audit while holding bootstrap flock");
        const QString journalReady = race.filePath(QStringLiteral("journal-b-ready"));
        const QString journalRelease = race.filePath(QStringLiteral("journal-b-release"));
        auto journal = startWorker({QStringLiteral("--stage1c-journal-worker"), root,
                                    journalReady, journalRelease, first, second, QStringLiteral("unused")});
        verify(QTest::qWaitFor([&] { return QFileInfo::exists(journalReady); }, 5000)
                   && readState(journalReady) == QByteArrayLiteral("blocked"),
               "B cannot begin or publish a journal while A owns final-audit-to-dispatch flock");
        verify(writeBarrier(journalRelease, QByteArrayLiteral("release")), "blocked B barrier is released");
        verify(journal->waitForFinished(5000) && journal->exitCode() == 3,
               "blocked journal worker exits without publishing");
        verify(writeBarrier(auditRelease, QByteArrayLiteral("release")), "A dispatch barrier is released");
        verify(audit->waitForFinished(5000) && audit->exitCode() == 0
                   && QFileInfo::exists(auditEffect)
                   && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "A performs one real dispatch and no competing journal is published");

        const QString journalReady2 = race.filePath(QStringLiteral("journal-b2-ready"));
        const QString journalRelease2 = race.filePath(QStringLiteral("journal-b2-release"));
        auto journal2 = startWorker({QStringLiteral("--stage1c-journal-worker"), root,
                                     journalReady2, journalRelease2, first, second, QStringLiteral("unused")});
        verify(QTest::qWaitFor([&] { return QFileInfo::exists(journalReady2); }, 5000)
                   && readState(journalReady2) == QByteArrayLiteral("published")
                   && !BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "B publishes its prepared journal while holding the shared flock");
        const QString auditReady2 = race.filePath(QStringLiteral("audit-a2-ready"));
        const QString auditRelease2 = race.filePath(QStringLiteral("audit-a2-release"));
        const QString auditEffect2 = race.filePath(QStringLiteral("audit-a2-effect"));
        auto audit2 = startWorker({QStringLiteral("--stage1c-audit-worker"), root,
                                   auditReady2, auditRelease2, auditEffect2, QStringLiteral("unused")});
        verify(QTest::qWaitFor([&] { return QFileInfo::exists(auditReady2); }, 5000)
                   && readState(auditReady2) == QByteArrayLiteral("blocked"),
               "A fails closed when B owns journal begin/publish flock");
        verify(writeBarrier(auditRelease2, QByteArrayLiteral("release")), "blocked A barrier is released");
        verify(audit2->waitForFinished(5000) && audit2->exitCode() == 3
                   && !QFileInfo::exists(auditEffect2),
               "reverse ordering causes zero unauthorized dispatch effects");
        verify(writeBarrier(journalRelease2, QByteArrayLiteral("release")), "published B barrier is released");
        verify(journal2->waitForFinished(5000) && journal2->exitCode() == 0
                   && BatchRenameRecoveryJournal::pendingJournal(root).isEmpty(),
               "normal work resumes after the journal lock and test journal are cleanly closed");
        BatchRenameRecoveryGate after(root);
        verify(!after.mutationsBlocked() && after.audit().inspectorLockHeld,
               "clean audit succeeds after shared lock expiry");
    }
    {
        QTemporaryDir locks;
        QFile stale(locks.filePath(QStringLiteral("cycle.lock")));
        verify(stale.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && stale.write("2147483647\n") > 0, "stale lock fixture"); stale.close();
        BatchRenameRecoveryGate staleGate(locks.path());
        verify(staleGate.audit().writerLock == BatchRenameRecoveryLock::None
                   && !staleGate.mutationsBlocked() && !QFileInfo::exists(stale.fileName()),
               "unambiguous stale writer lock without a journal is safely reclaimed");
    }
    {
        QTemporaryDir locks;
        QFile ambiguous(locks.filePath(QStringLiteral("cycle.lock")));
        verify(ambiguous.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && ambiguous.write("not-a-pid\n") > 0, "ambiguous lock fixture"); ambiguous.close();
        BatchRenameRecoveryGate ambiguousGate(locks.path());
        verify(ambiguousGate.audit().writerLock == BatchRenameRecoveryLock::Ambiguous
                   && ambiguousGate.mutationsBlocked(), "malformed writer lock is ambiguous and fails closed");
    }
    {
        QTemporaryDir attacks;
        verify(attacks.isValid(), "isolated recovery path attack fixtures");
        const QString target = attacks.filePath(QStringLiteral("target"));
        QFile targetFile(target);
        verify(targetFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                   && targetFile.write("unchanged") == 9,
               "lock symlink target fixture is created");
        targetFile.close();
        const QString symlinkRoot = attacks.filePath(QStringLiteral("symlink-root"));
        verify(::symlink(QFile::encodeName(attacks.path()).constData(),
                         QFile::encodeName(symlinkRoot).constData()) == 0,
               "recovery root symlink fixture is created");
        BatchRenameRecoveryGate symlinkPath(symlinkRoot + QStringLiteral("/nested"));
        verify(symlinkPath.mutationsBlocked()
                   && !QFileInfo::exists(attacks.filePath(QStringLiteral("nested"))),
               "bootstrap refuses a symlinked path component without creating through it");
        const QString lockRoot = attacks.filePath(QStringLiteral("lock-root"));
        verify(QDir().mkdir(lockRoot)
                   && ::symlink(QFile::encodeName(target).constData(),
                                QFile::encodeName(lockRoot + QStringLiteral("/recovery-audit.lock")).constData()) == 0,
               "recovery lock symlink fixture is created");
        BatchRenameRecoveryGate symlinkLock(lockRoot);
        QFile unchanged(target);
        verify(symlinkLock.mutationsBlocked() && unchanged.open(QIODevice::ReadOnly)
                   && unchanged.readAll() == QByteArrayLiteral("unchanged"),
               "bootstrap refuses a lock symlink and never overwrites its target");
        const QString fileRoot = attacks.filePath(QStringLiteral("not-a-directory"));
        QFile rootFile(fileRoot);
        verify(rootFile.open(QIODevice::WriteOnly | QIODevice::NewOnly),
               "non-directory root fixture is created");
        rootFile.close();
        BatchRenameRecoveryGate invalidRoot(fileRoot);
        verify(invalidRoot.mutationsBlocked(), "non-directory recovery root fails closed");
    }
    verify(QDir().mkdir(files.filePath(QStringLiteral("folder"))), "directory fixture");
    const QUrl folder = QUrl::fromLocalFile(files.filePath(QStringLiteral("folder")));

    auto plan = makeBatchRenamePlan({beta, alpha, folder}, QStringLiteral("pre-"),
                                    QStringLiteral("-done"), true, 7, 3);
    verify(plan.isValid() && plan.entries.size() == 3, "mixed selection has a valid plan");
    verify(plan.entries.at(0).oldName == QStringLiteral("alpha.txt")
               && plan.entries.at(0).newName == QStringLiteral("pre-alpha-done 007.txt"),
           "order and file extension preview are deterministic");
    verify(plan.entries.at(2).oldName == QStringLiteral("folder")
               && plan.entries.at(2).newName == QStringLiteral("pre-folder-done 009"),
           "directories do not gain a file extension");

    auto unchanged = makeBatchRenamePlan({alpha, beta}, {}, {}, false, 1, 2);
    verify(!unchanged.isValid() && unchanged.activeCount() == 0
               && unchanged.errorRow == -1,
           "an all-no-op selection is rejected without marking a row as erroneous");
    auto invalid = makeBatchRenamePlan({alpha, beta}, QStringLiteral("bad/"), {}, false, 1, 2);
    verify(!invalid.isValid(), "slash in a generated name is rejected");

    const QUrl existing = makeFile(QStringLiteral("pre-alpha.txt"));
    Q_UNUSED(existing);
    auto conflict = makeBatchRenamePlan({alpha, beta}, QStringLiteral("pre-"), {}, false, 1, 2);
    verify(!conflict.isValid(), "existing destination prevents overwrite");

    const QUrl upper = makeFile(QStringLiteral("UPPER"));
    const QUrl lower = makeFile(QStringLiteral("upper"));
    auto folded = makeBatchRenamePlan({upper, lower}, QStringLiteral("x"), {}, false, 1, 2);
    verify(!folded.isValid(), "case-folded result collisions are rejected conservatively");

    const QString linkPath = files.filePath(QStringLiteral("link"));
    verify(::symlink(QFile::encodeName(alpha.toLocalFile()).constData(),
                     QFile::encodeName(linkPath).constData()) == 0,
           "symlink fixture");
    auto links = makeBatchRenamePlan({alpha, QUrl::fromLocalFile(linkPath)},
                                     QStringLiteral("safe-"), {}, false, 1, 2);
    verify(links.isValid() && links.entries.at(1).symlink,
           "symbolic links are planned as links, not followed as payloads");
    verify(batchRenameSourceUnchanged(links.entries.first()), "source snapshot initially matches");
    QFile mutate(alpha.toLocalFile());
    verify(mutate.open(QIODevice::Append) && mutate.write("changed") == 7, "source mutation fixture");
    mutate.close();
    verify(!batchRenameSourceUnchanged(links.entries.first()),
           "filesystem changes after preview are detected");
    const QUrl replaced = makeFile(QStringLiteral("identity"));
    auto identity = makeBatchRenamePlan({beta, replaced}, QStringLiteral("id-"), {}, false, 1, 2);
    verify(identity.isValid(), "identity fixture has valid preview");
    verify(QFile::rename(replaced.toLocalFile(), files.filePath(QStringLiteral("identity.old"))),
           "original inode is kept under a different test name");
    QFile replacement(replaced.toLocalFile());
    verify(replacement.open(QIODevice::WriteOnly), "replacement fixture opens");
    verify(replacement.write("different") == 9, "replacement fixture writes");
    replacement.close();
    verify(!batchRenamePlanReady(identity), "replaced inode blocks complete preflight");

    auto race = makeBatchRenamePlan({beta, folder}, QStringLiteral("race-"), {}, false, 1, 2);
    verify(race.isValid() && batchRenameEntryReady(race.entries.first()),
           "execution preflight initially accepts an unchanged entry");
    QFile lateDestination(race.entries.first().destination.toLocalFile());
    verify(lateDestination.open(QIODevice::WriteOnly), "late conflict fixture opens");
    lateDestination.close();
    verify(!batchRenameEntryReady(race.entries.first()),
           "a destination created after preview prevents execution and overwrite");

    BatchRenameOptions literalOptions;
    literalOptions.findText = QStringLiteral("a");
    literalOptions.replacementText = QStringLiteral("XY");
    auto literal = makeBatchRenamePlan({alpha, beta}, literalOptions);
    verify(literal.isValid()
               && literal.entries.first().newName == QStringLiteral("XYlphXY.txt")
               && literal.entries.last().newName == QStringLiteral("betXY.txt"),
           "literal replacement affects matching stems only");
    literalOptions.findText.clear();
    auto emptyLiteral = makeBatchRenamePlan({alpha, beta}, literalOptions);
    verify(!emptyLiteral.isValid()
               && emptyLiteral.entries.first().newName == QStringLiteral("alpha.txt"),
           "empty literal pattern is a no-op rather than insertion between characters");

    BatchRenameOptions caseOptions;
    caseOptions.letterCase = BatchRenameCase::Upper;
    auto letterCase = makeBatchRenamePlan({beta, folder}, caseOptions);
    verify(letterCase.isValid()
               && letterCase.entries.first().newName == QStringLiteral("BETA.txt")
               && letterCase.entries.last().newName == QStringLiteral("FOLDER"),
           "case transformation changes stems and supports directories");

    const QUrl dotfile = makeFile(QStringLiteral(".profile"));
    BatchRenameOptions extensionOptions;
    extensionOptions.extensionMode = BatchRenameExtension::Replace;
    extensionOptions.extension = QStringLiteral(".md");
    extensionOptions.prefix = QStringLiteral("x-");
    auto extensions = makeBatchRenamePlan({beta, dotfile, folder}, extensionOptions);
    verify(extensions.isValid()
               && extensions.entries.at(0).newName == QStringLiteral("x-.profile.md")
               && extensions.entries.at(1).newName == QStringLiteral("x-beta.md")
               && extensions.entries.at(2).newName == QStringLiteral("x-folder"),
           "extension replacement treats dotfiles as stems and ignores directories");
    extensionOptions.extensionMode = BatchRenameExtension::Remove;
    extensionOptions.prefix.clear();
    auto removedExtensions = makeBatchRenamePlan({beta, dotfile}, extensionOptions);
    verify(removedExtensions.isValid() && removedExtensions.activeCount() == 1
               && removedExtensions.executionOrder.size() == 1
               && removedExtensions.entries.first().noOp
               && removedExtensions.entries.first().newName == QStringLiteral(".profile")
               && removedExtensions.entries.last().newName == QStringLiteral("beta"),
           "extension removal skips an unchanged dotfile while scheduling the changed file");

    QTemporaryDir mixedExtensionFiles;
    verify(mixedExtensionFiles.isValid(), "mixed extension fixture is isolated");
    const auto mixedFile = [&](const QString &name, const QByteArray &payload) {
        QFile file(mixedExtensionFiles.filePath(name));
        verify(file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size(),
               "mixed extension fixture writes payload");
        file.close();
        return QUrl::fromLocalFile(file.fileName());
    };
    const QUrl archive = mixedFile(QStringLiteral("a.tar.gz"), QByteArrayLiteral("archive"));
    const QUrl readme = mixedFile(QStringLiteral("README"), QByteArrayLiteral("readme"));
    const QUrl mixedDotfile = mixedFile(QStringLiteral(".profile"), QByteArrayLiteral("profile"));
    verify(QDir().mkdir(mixedExtensionFiles.filePath(QStringLiteral("Folder"))),
           "mixed extension fixture creates directory");
    QFile nested(mixedExtensionFiles.filePath(QStringLiteral("Folder/data.txt")));
    verify(nested.open(QIODevice::WriteOnly) && nested.write("nested") == 6,
           "mixed extension fixture writes nested content");
    nested.close();
    const QUrl mixedFolder = QUrl::fromLocalFile(
        mixedExtensionFiles.filePath(QStringLiteral("Folder")));
    auto mixedRemoval = makeBatchRenamePlan(
        {archive, readme, mixedDotfile, mixedFolder}, extensionOptions);
    verify(mixedRemoval.isValid() && mixedRemoval.entries.size() == 4
               && mixedRemoval.activeCount() == 1
               && mixedRemoval.executionOrder.size() == 1,
           "one extension change plus directory/dotfile/extensionless no-ops is valid");
    int skippedRows = 0;
    for (const auto &entry : std::as_const(mixedRemoval.entries)) skippedRows += entry.noOp;
    const auto &activeRemoval = mixedRemoval.entries.at(mixedRemoval.executionOrder.first());
    verify(skippedRows == 3 && activeRemoval.oldName == QStringLiteral("a.tar.gz")
               && activeRemoval.newName == QStringLiteral("a.tar"),
           "only the real extension change enters executionOrder");
    verify(batchRenamePlanReady(mixedRemoval) && batchRenameEntryReady(activeRemoval)
               && QFile::rename(activeRemoval.source.toLocalFile(),
                                activeRemoval.destination.toLocalFile()),
           "mixed plan preflight executes only its active row");
    QFile archivePayload(activeRemoval.destination.toLocalFile());
    QFile nestedPayload(mixedExtensionFiles.filePath(QStringLiteral("Folder/data.txt")));
    verify(archivePayload.open(QIODevice::ReadOnly)
               && archivePayload.readAll() == QByteArrayLiteral("archive")
               && nestedPayload.open(QIODevice::ReadOnly)
               && nestedPayload.readAll() == QByteArrayLiteral("nested")
               && QFileInfo::exists(readme.toLocalFile())
               && QFileInfo::exists(mixedDotfile.toLocalFile()),
           "mixed execution preserves active and skipped contents");
    verify(!batchRenameLinearHistoryEligible(mixedRemoval),
           "one active row uses one ordinary KIO Undo and creates no no-op history entries");

    QTemporaryDir mixedHistoryFiles;
    QTemporaryDir mixedHistoryJournal;
    verify(mixedHistoryFiles.isValid() && mixedHistoryJournal.isValid(),
           "mixed grouped-history fixtures are isolated");
    QList<QUrl> mixedHistorySources;
    for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("two.txt"),
                                QStringLiteral("README"), QStringLiteral(".profile")}) {
        QFile file(mixedHistoryFiles.filePath(name));
        verify(file.open(QIODevice::WriteOnly)
                   && file.write(name.toUtf8()) == name.toUtf8().size(),
               "mixed grouped-history fixture writes identifiable content");
        file.close();
        mixedHistorySources.push_back(QUrl::fromLocalFile(file.fileName()));
    }
    auto mixedHistoryPlan = makeBatchRenamePlan(mixedHistorySources, extensionOptions);
    verify(mixedHistoryPlan.isValid() && mixedHistoryPlan.activeCount() == 2
               && mixedHistoryPlan.executionOrder.size() == 2
               && batchRenameLinearHistoryEligible(mixedHistoryPlan),
           "two real changes plus no-ops remain eligible for grouped history");
    const auto mixedHistoryRun = batchRenameReplayLinear(
        mixedHistoryPlan, false, mixedHistoryJournal.path());
    const auto mixedHistoryUndo = batchRenameReplayLinear(
        mixedHistoryPlan, true, mixedHistoryJournal.path());
    const auto mixedHistoryRedo = batchRenameReplayLinear(
        mixedHistoryPlan, false, mixedHistoryJournal.path());
    QFile redoneOne(mixedHistoryFiles.filePath(QStringLiteral("one")));
    QFile redoneTwo(mixedHistoryFiles.filePath(QStringLiteral("two")));
    verify(mixedHistoryRun.success && mixedHistoryUndo.success && mixedHistoryRedo.success
               && redoneOne.open(QIODevice::ReadOnly)
               && redoneOne.readAll() == QByteArrayLiteral("one.txt")
               && redoneTwo.open(QIODevice::ReadOnly)
               && redoneTwo.readAll() == QByteArrayLiteral("two.txt")
               && QFileInfo::exists(mixedHistoryFiles.filePath(QStringLiteral("README")))
               && QFileInfo::exists(mixedHistoryFiles.filePath(QStringLiteral(".profile"))),
           "grouped execution and Undo/Redo preserve active payloads and skip no-ops");

    const QUrl held = mixedFile(QStringLiteral("hold"), QByteArrayLiteral("held"));
    const QUrl moving = mixedFile(QStringLiteral("move"), QByteArrayLiteral("moving"));
    BatchRenameOptions selectedNoOpConflictOptions;
    selectedNoOpConflictOptions.findText = QStringLiteral("move");
    selectedNoOpConflictOptions.replacementText = QStringLiteral("hold");
    auto selectedNoOpConflict = makeBatchRenamePlan({held, moving}, selectedNoOpConflictOptions);
    verify(!selectedNoOpConflict.isValid() && selectedNoOpConflict.activeCount() == 1
               && (selectedNoOpConflict.error.contains(QStringLiteral("zaznaczonym"))
                   || selectedNoOpConflict.error.contains(QStringLiteral("selected"))),
           "an active row cannot overwrite a selected no-op source");

    BatchRenameOptions regexOptions;
    regexOptions.regularExpression = true;
    regexOptions.findText = QStringLiteral("^(.*)a$");
    regexOptions.replacementText = QStringLiteral("\\1-Z");
    auto regex = makeBatchRenamePlan({alpha, beta}, regexOptions);
    verify(regex.isValid()
               && regex.entries.first().newName == QStringLiteral("alph-Z.txt")
               && regex.entries.last().newName == QStringLiteral("bet-Z.txt"),
           "validated regex replacement supports capture references");
    regexOptions.findText = QStringLiteral("[");
    auto invalidRegex = makeBatchRenamePlan({alpha, beta}, regexOptions);
    verify(!invalidRegex.isValid() && invalidRegex.error.contains(QStringLiteral("regular"), Qt::CaseInsensitive),
           "invalid regex is rejected with a useful message");
    regexOptions.findText.clear();
    auto emptyRegex = makeBatchRenamePlan({alpha, beta}, regexOptions);
    verify(!emptyRegex.isValid(), "empty regex is rejected instead of matching every position");

    const QUrl one = makeFile(QStringLiteral("1"));
    const QUrl two = makeFile(QStringLiteral("2"));
    const QUrl fortyOne = makeFile(QStringLiteral("41"));
    Q_UNUSED(fortyOne);
    auto screenshotConflict = makeBatchRenamePlan({one, two}, QStringLiteral("4"), {}, false, 1, 2);
    verify(!screenshotConflict.isValid() && screenshotConflict.entries.size() == 2,
           "prefix-4 conflict keeps the complete screenshot preview");
    verify(screenshotConflict.errorRow == 0
               && screenshotConflict.entries.first().newName == QStringLiteral("41")
               && (screenshotConflict.entries.first().problem.contains(QStringLiteral("spoza zaznaczenia"))
                   || screenshotConflict.entries.first().problem.contains(QStringLiteral("unselected"))),
           "prefix-4 conflict identifies row 1 and the unselected existing item");
    // 1 -> 41 and 41 -> 441: the latter must run first to vacate 41.
    auto selectedChain = makeBatchRenamePlan({one, fortyOne}, QStringLiteral("4"), {}, false, 1, 2);
    verify(selectedChain.isValid() && selectedChain.executionOrder == QList<int>({1, 0}),
           "selected destination chain is valid and executes vacancy first");
    verify(batchRenamePlanReady(selectedChain), "complete chain preflight accepts unchanged snapshot");
    verify(!batchRenameEntryReady(selectedChain.entries.first()),
           "individual preflight still refuses to overwrite an occupied source");
    const QUrl eleven = makeFile(QStringLiteral("11"));
    const QUrl oneEleven = makeFile(QStringLiteral("111"));
    auto longerChain = makeBatchRenamePlan({one, eleven, oneEleven},
                                           QStringLiteral("1"), {}, false, 1, 2);
    verify(longerChain.isValid() && longerChain.executionOrder == QList<int>({2, 1, 0}),
           "three-element chain has deterministic vacancy-first execution order");
    BatchRenameOptions cycleOptions;
    cycleOptions.regularExpression = true;
    cycleOptions.findText = QStringLiteral("^(.)(.)$");
    cycleOptions.replacementText = QStringLiteral("\\2\\1");
    const QUrl ab = makeFile(QStringLiteral("ab"));
    const QUrl ba = makeFile(QStringLiteral("ba"));
    auto cycle = makeBatchRenamePlan({ab, ba}, cycleOptions);
    verify(cycle.isValid() && cycle.executionOrder.isEmpty()
               && cycle.atomicSwaps.size() == 1,
           "mutual two-way dependency is planned as one atomic exchange");
    verify(batchRenameSwapReady(cycle, cycle.atomicSwaps.first().first,
                                cycle.atomicSwaps.first().second),
           "atomic swap preflight verifies both selected originals");
    QString exchangeError;
    verify(batchRenameAtomicSwap(cycle, cycle.atomicSwaps.first().first,
                                 cycle.atomicSwaps.first().second, &exchangeError),
           "Linux renameat2 exchanges both file names in one operation");
    QFile readAb(ab.toLocalFile());
    QFile readBa(ba.toLocalFile());
    verify(readAb.open(QIODevice::ReadOnly) && readAb.readAll() == QByteArrayLiteral("ba")
               && readBa.open(QIODevice::ReadOnly) && readBa.readAll() == QByteArrayLiteral("ab"),
           "atomic exchange preserves both distinct file contents");
    readAb.close();
    readBa.close();
    verify(!batchRenameSwapReady(cycle, cycle.atomicSwaps.first().first,
                                 cycle.atomicSwaps.first().second),
           "stale snapshot cannot swap the same pair a second time");
    auto reversedSwap = makeBatchRenamePlan({ab, ba}, cycleOptions);
    verify(reversedSwap.isValid() && batchRenameAtomicSwap(reversedSwap,
               reversedSwap.atomicSwaps.first().first,
               reversedSwap.atomicSwaps.first().second, &exchangeError),
           "a fresh snapshot can exchange the pair back without temporary files");
    verify(batchRenameSourceUnchanged(cycle.entries.first())
               && batchRenameSourceUnchanged(cycle.entries.last()),
           "both original inodes return to their original names");
    const QUrl abc = makeFile(QStringLiteral("abc"));
    const QUrl bca = makeFile(QStringLiteral("bca"));
    const QUrl cab = makeFile(QStringLiteral("cab"));
    BatchRenameOptions tripleOptions;
    tripleOptions.regularExpression = true;
    tripleOptions.findText = QStringLiteral("^(.)(..)$");
    tripleOptions.replacementText = QStringLiteral("\\2\\1");
    auto triple = makeBatchRenamePlan({abc, bca, cab}, tripleOptions);
    verify(triple.isValid() && triple.exchangeCycles.size() == 1
               && triple.exchangeCycles.first().size() == 3
               && triple.atomicSwaps.isEmpty() && triple.executionOrder.isEmpty(),
           "three-way dependency is recognized as one isolated cycle");
    const auto tripleCycle = triple.exchangeCycles.first();
    // Crash-inspection journal test: no real user files are touched.  A
    // surviving JSON manifest blocks subsequent cycles until inspected.
    QTemporaryDir recoveryDir;
    verify(recoveryDir.isValid(), "isolated recovery journal directory");
    QString recoveryError;
    {
        BatchRenameRecoveryJournal journal(recoveryDir.path());
        verify(journal.begin(triple, tripleCycle, &recoveryError),
               "cycle journal is synchronized before the first exchange");
        const QString path = BatchRenameRecoveryJournal::pendingJournal(recoveryDir.path());
        verify(!path.isEmpty() && path == journal.path(),
               "pending cycle journal is discoverable");
        QFile saved(path);
        verify(saved.open(QIODevice::ReadOnly), "journal can be read for inspection");
        const QJsonObject record = QJsonDocument::fromJson(saved.readAll()).object();
        saved.close();
        verify(record.value(QStringLiteral("originals")).toArray().size() == 3
                   && record.value(QStringLiteral("cycleRows")).toArray().size() == 3
                   && record.value(QStringLiteral("originals")).toArray().at(0).toObject()
                       .value(QStringLiteral("row")).isDouble()
                   && record.value(QStringLiteral("phase")).toString()
                       == QStringLiteral("prepared")
                   && record.value(QStringLiteral("originals")).toArray().at(0).toObject()
                       .value(QStringLiteral("inode")).isString(),
               "journal captures exact 64-bit identities and phase");
        BatchRenameRecoveryJournal concurrent(recoveryDir.path());
        verify(!concurrent.begin(triple, tripleCycle, &recoveryError),
               "parallel cycle cannot race the active recovery journal");
        auto unchangedState = batchRenameCycleInitialState(tripleCycle);
        verify(journal.checkpoint(QStringLiteral("exchange-intent"), unchangedState,
                                  &recoveryError),
               "exchange intent persisted before syscall");
        verify(journal.finish(QStringLiteral("verified-rollback"), unchangedState,
                              &recoveryError)
                   && BatchRenameRecoveryJournal::pendingJournal(recoveryDir.path()).isEmpty(),
               "verified restoration cleans up the journal");
    }
    auto partialState = batchRenameCycleInitialState(tripleCycle);
    {
        BatchRenameRecoveryJournal interrupted(recoveryDir.path());
        verify(interrupted.begin(triple, tripleCycle, &recoveryError),
               "second fixture starts a durable cycle journal");
        verify(interrupted.checkpoint(QStringLiteral("exchange-intent"), partialState,
                                      &recoveryError), "intent precedes the real exchange");
        verify(batchRenameCycleAdvance(triple, tripleCycle, partialState, &recoveryError)
                   && partialState.stepsDone == 1,
               "real exchange creates a partial three-way cycle");
        verify(interrupted.checkpoint(QStringLiteral("exchange-verified"), partialState,
                                      &recoveryError), "partial permutation is journaled");
    } // Simulate unexpected process exit: no automatic deletion in destructor.
    const QString unresolved = BatchRenameRecoveryJournal::pendingJournal(recoveryDir.path());
    verify(!unresolved.isEmpty(), "journal survives destruction for post-crash inspection");
    {
        BatchRenameRecoveryJournal blocked(recoveryDir.path());
        verify(!blocked.begin(triple, tripleCycle, &recoveryError)
                   && QFileInfo::exists(unresolved),
               "unresolved journal blocks even a later process");
    }
    verify(batchRenameCycleMatches(triple, tripleCycle, partialState),
           "partial inode mapping is still identifiable after process-like teardown");
    verify(batchRenameCycleRollback(triple, tripleCycle, partialState, &recoveryError),
           "isolated test restores original inode mapping before clearing evidence");
    verify(QFile::remove(unresolved), "only the verified test fixture clears its journal");
    verify(batchRenameCycleReady(triple, tripleCycle),
           "all three original inodes match before the first exchange");
    auto tripleState = batchRenameCycleInitialState(tripleCycle);
    verify(batchRenameCycleAdvance(triple, tripleCycle, tripleState, &exchangeError)
               && tripleState.stepsDone == 1
               && !batchRenameCycleComplete(triple, tripleCycle, tripleState),
           "the first exchange leaves a known intermediate permutation");
    verify(batchRenameCycleRollback(triple, tripleCycle, tripleState, &exchangeError)
               && tripleState.stepsDone == 0
               && batchRenameCycleReady(triple, tripleCycle),
           "cancel after first exchange restores all three original entries");
    verify(batchRenameCycleAdvance(triple, tripleCycle, tripleState, &exchangeError)
               && batchRenameCycleAdvance(triple, tripleCycle, tripleState, &exchangeError)
               && batchRenameCycleComplete(triple, tripleCycle, tripleState),
           "two successive atomic exchanges complete a three-way cycle");
    QFile tripleA(abc.toLocalFile());
    QFile tripleB(bca.toLocalFile());
    QFile tripleC(cab.toLocalFile());
    verify(tripleA.open(QIODevice::ReadOnly) && tripleA.readAll() == QByteArrayLiteral("cab")
               && tripleB.open(QIODevice::ReadOnly) && tripleB.readAll() == QByteArrayLiteral("abc")
               && tripleC.open(QIODevice::ReadOnly) && tripleC.readAll() == QByteArrayLiteral("bca"),
           "three-cycle rotates all file contents without overwriting any file");
    tripleA.close();
    tripleB.close();
    tripleC.close();
    verify(!batchRenameCycleAdvance(triple, tripleCycle, tripleState, &exchangeError),
           "completed cycle cannot accidentally execute an extra exchange");

    // Stage 3C.2B.1: one completed, isolated cycle can be replayed in BOTH
    // directions using a fresh durable journal per Undo/Redo operation.
    verify(batchRenameSingleCycleHistoryEligible(triple)
               && batchRenameCycleSnapshotsMatch(triple, tripleCycle,
                                                  batchRenameCycleFinalState(tripleCycle)),
           "one completed three-cycle has a verifiable history snapshot");
    QTemporaryDir replayRoot;
    verify(replayRoot.isValid(), "isolated Undo/Redo recovery journal directory");
    const auto midReplayError = batchRenameReplaySingleCycle(triple, true,
                                                              replayRoot.path(), 1);
    verify(!midReplayError.success && !midReplayError.uncertain
               && BatchRenameRecoveryJournal::pendingJournal(replayRoot.path()).isEmpty()
               && batchRenameCycleSnapshotsMatch(triple, tripleCycle,
                                                 batchRenameCycleFinalState(tripleCycle)),
           "injected failure after one Undo step restores exact initial permutation and clears journal");
    const auto replayUndo = batchRenameReplaySingleCycle(triple, true, replayRoot.path());
    verify(replayUndo.success && !replayUndo.uncertain
               && BatchRenameRecoveryJournal::pendingJournal(replayRoot.path()).isEmpty()
               && batchRenameCycleReady(triple, tripleCycle),
           "one journaled Undo reverses all three names without a leftover manifest");
    for (const auto &entry : std::as_const(triple.entries)) {
        QFile original(entry.source.toLocalFile());
        verify(original.open(QIODevice::ReadOnly)
                   && original.readAll() == entry.oldName.toUtf8(),
               "Undo restores each original payload at its original pathname");
    }
    const auto replayRedo = batchRenameReplaySingleCycle(triple, false, replayRoot.path());
    verify(replayRedo.success && !replayRedo.uncertain
               && BatchRenameRecoveryJournal::pendingJournal(replayRoot.path()).isEmpty()
               && batchRenameCycleSnapshotsMatch(triple, tripleCycle,
                                                 batchRenameCycleFinalState(tripleCycle)),
           "one journaled Redo repeats all three names without a leftover manifest");
    for (const auto &entry : std::as_const(triple.entries)) {
        QFile moved(entry.destination.toLocalFile());
        verify(moved.open(QIODevice::ReadOnly)
                   && moved.readAll() == entry.oldName.toUtf8(),
               "Redo preserves every payload at its intended destination");
    }
    const QString stopPath = replayRoot.filePath(QStringLiteral("cycle-blocking.json"));
    QFile stopFile(stopPath);
    verify(stopFile.open(QIODevice::WriteOnly)
               && stopFile.write("UNRESOLVED") == 10,
           "test-only unresolved recovery manifest is created");
    stopFile.close();
    const auto refusedReplay = batchRenameReplaySingleCycle(triple, true, replayRoot.path());
    verify(!refusedReplay.success && !refusedReplay.uncertain
               && QFileInfo::exists(stopPath)
               && batchRenameCycleSnapshotsMatch(triple, tripleCycle,
                                                 batchRenameCycleFinalState(tripleCycle)),
           "unresolved journal refuses a new history replay without changing any inode");
    verify(QFile::remove(stopPath), "only test-created journal evidence is removed");

    // A four-item cycle must use three exchanges. Different content in each
    // item proves permutation direction and ensures no payload was discarded.
    QTemporaryDir fourCycleDir;
    verify(fourCycleDir.isValid(), "isolated four-cycle fixture");
    QList<QUrl> fourUrls;
    for (const QString &name : {QStringLiteral("abcd"), QStringLiteral("bcda"),
                                QStringLiteral("cdab"), QStringLiteral("dabc")}) {
        QFile item(fourCycleDir.filePath(name));
        verify(item.open(QIODevice::WriteOnly) && item.write(name.toUtf8()) == 4,
               "four-cycle fixture has unique content");
        item.close();
        fourUrls.push_back(QUrl::fromLocalFile(item.fileName()));
    }
    BatchRenameOptions fourOptions;
    fourOptions.regularExpression = true;
    fourOptions.findText = QStringLiteral("^(.)(...)$");
    fourOptions.replacementText = QStringLiteral("\\2\\1");
    auto fourPlan = makeBatchRenamePlan(fourUrls, fourOptions);
    verify(fourPlan.isValid() && fourPlan.exchangeCycles.size() == 1
               && fourPlan.exchangeCycles.first().size() == 4,
           "four-cycle is planned with deterministic ordered dependencies");
    const auto fourCycle = fourPlan.exchangeCycles.first();
    auto fourState = batchRenameCycleInitialState(fourCycle);
    verify(batchRenameCycleReady(fourPlan, fourCycle), "four-cycle complete preflight");
    for (int step = 0; step < 3; ++step)
        verify(batchRenameCycleAdvance(fourPlan, fourCycle, fourState, &exchangeError),
               "one verified atomic exchange succeeds in four-cycle");
    verify(batchRenameCycleComplete(fourPlan, fourCycle, fourState),
           "four-cycle reaches exactly all four desired destinations");
    for (const auto &entry : std::as_const(fourPlan.entries)) {
        QFile file(entry.destination.toLocalFile());
        verify(file.open(QIODevice::ReadOnly) && file.readAll() == entry.oldName.toUtf8(),
               "original four-cycle contents follow their respective destination URLs");
    }
    verify(QDir(fourCycleDir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).size() == 4,
           "no temporary or extra files are left by exchange-only cycles");

    // No rollback may exchange an unknown inode: a concurrent program might
    // have moved a filename aside and put its own file at the same path.
    QTemporaryDir interruptedDir;
    verify(interruptedDir.isValid(), "isolated concurrent-change fixture");
    QList<QUrl> interruptedUrls;
    for (const QString &name : {QStringLiteral("abc"), QStringLiteral("bca"),
                                QStringLiteral("cab")}) {
        QFile item(interruptedDir.filePath(name));
        verify(item.open(QIODevice::WriteOnly) && item.write(name.toUtf8()) == 3,
               "interrupted-cycle fixture has unique content");
        item.close();
        interruptedUrls.push_back(QUrl::fromLocalFile(item.fileName()));
    }
    auto interrupted = makeBatchRenamePlan(interruptedUrls, tripleOptions);
    verify(interrupted.isValid(), "interrupted cycle preview is valid");
    const auto interruptedCycle = interrupted.exchangeCycles.first();
    auto interruptedState = batchRenameCycleInitialState(interruptedCycle);
    verify(batchRenameCycleAdvance(interrupted, interruptedCycle, interruptedState, &exchangeError),
           "first exchange before external interference succeeds");
    const QString changedPath = interrupted.entries.at(interruptedCycle.at(2)).source.toLocalFile();
    verify(QFile::rename(changedPath, changedPath + QStringLiteral(".aside")),
           "other process moves a cycle entry aside during intermediate state");
    QFile outsider(changedPath);
    verify(outsider.open(QIODevice::WriteOnly) && outsider.write("OUTSIDER") == 8,
           "other process inserts an unrelated inode at the vacated path");
    outsider.close();
    verify(!batchRenameCycleAdvance(interrupted, interruptedCycle, interruptedState, &exchangeError)
               && !batchRenameCycleRollback(interrupted, interruptedCycle, interruptedState, &exchangeError),
           "changed identity blocks both forward execution and unsafe rollback");
    QFile outsiderRead(changedPath);
    verify(outsiderRead.open(QIODevice::ReadOnly)
               && outsiderRead.readAll() == QByteArrayLiteral("OUTSIDER")
               && QFileInfo::exists(changedPath + QStringLiteral(".aside")),
           "unknown inode is never exchanged or overwritten by rollback");
    outsiderRead.close();
    const QUrl swapLeft = makeFile(QStringLiteral("xy"));
    const QUrl swapRight = makeFile(QStringLiteral("yx"));
    auto staleSwap = makeBatchRenamePlan({swapLeft, swapRight}, cycleOptions);
    verify(staleSwap.isValid(), "second pair has a valid preview");
    verify(QFile::rename(swapLeft.toLocalFile(), files.filePath(QStringLiteral("xy.old"))),
           "swap race fixture moves source aside");
    QFile replacedSwap(swapLeft.toLocalFile());
    verify(replacedSwap.open(QIODevice::WriteOnly) && replacedSwap.write("outsider") == 8,
           "swap race fixture creates a replacement inode");
    replacedSwap.close();
    verify(!batchRenameSwapReady(staleSwap, staleSwap.atomicSwaps.first().first,
                                 staleSwap.atomicSwaps.first().second),
           "substituting an inode after preview blocks the atomic exchange");
    // An exchange of two symlinks must move the link inodes, not their targets.
    const QString linkCd = files.filePath(QStringLiteral("cd"));
    const QString linkDc = files.filePath(QStringLiteral("dc"));
    verify(::symlink(QFile::encodeName(alpha.toLocalFile()).constData(),
                     QFile::encodeName(linkCd).constData()) == 0
               && ::symlink(QFile::encodeName(beta.toLocalFile()).constData(),
                            QFile::encodeName(linkDc).constData()) == 0,
           "two-way symlink fixture opens");
    const QString initialCdTarget = QFileInfo(linkCd).symLinkTarget();
    const QString initialDcTarget = QFileInfo(linkDc).symLinkTarget();
    auto linkSwap = makeBatchRenamePlan({QUrl::fromLocalFile(linkCd),
                                         QUrl::fromLocalFile(linkDc)}, cycleOptions);
    verify(linkSwap.isValid() && batchRenameAtomicSwap(linkSwap,
               linkSwap.atomicSwaps.first().first, linkSwap.atomicSwaps.first().second),
           "atomic swap supports symlinks without following targets");
    verify(QFileInfo(linkCd).isSymLink() && QFileInfo(linkDc).isSymLink()
               && QFileInfo(linkCd).symLinkTarget() == initialDcTarget
               && QFileInfo(linkDc).symLinkTarget() == initialCdTarget,
           "symlink inodes exchanged while their targets remain intact");

    // Same-parent directories are exchanged as directory entries, not copied.
    const QString dirEf = files.filePath(QStringLiteral("ef"));
    const QString dirFe = files.filePath(QStringLiteral("fe"));
    verify(QDir().mkdir(dirEf) && QDir().mkdir(dirFe), "directory swap fixtures open");
    QFile dirChildEf(QDir(dirEf).filePath(QStringLiteral("first.txt")));
    QFile dirChildFe(QDir(dirFe).filePath(QStringLiteral("second.txt")));
    verify(dirChildEf.open(QIODevice::WriteOnly) && dirChildEf.write("first") == 5,
           "first directory content fixture");
    dirChildEf.close();
    verify(dirChildFe.open(QIODevice::WriteOnly) && dirChildFe.write("second") == 6,
           "second directory content fixture");
    dirChildFe.close();
    auto folderSwap = makeBatchRenamePlan({QUrl::fromLocalFile(dirEf),
                                           QUrl::fromLocalFile(dirFe)}, cycleOptions);
    verify(folderSwap.isValid() && batchRenameAtomicSwap(folderSwap,
               folderSwap.atomicSwaps.first().first, folderSwap.atomicSwaps.first().second),
           "atomic exchange supports sibling directories");
    verify(QFileInfo::exists(QDir(dirEf).filePath(QStringLiteral("second.txt")))
               && QFileInfo::exists(QDir(dirFe).filePath(QStringLiteral("first.txt"))),
           "both directory contents survive the exchange");

    const QUrl occupied = makeFile(QStringLiteral("1111"));
    Q_UNUSED(occupied);
    verify(!batchRenamePlanReady(longerChain),
           "late external collision blocks entire chain before the first rename");

    QTemporaryDir chainFiles;
    verify(chainFiles.isValid(), "isolated execution-order fixture");
    const QString chainFirst = chainFiles.filePath(QStringLiteral("1"));
    const QString chainSecond = chainFiles.filePath(QStringLiteral("41"));
    for (const QString &name : {chainFirst, chainSecond}) {
        QFile fixture(name);
        verify(fixture.open(QIODevice::WriteOnly), "chain fixture opens");
        verify(fixture.write(QFileInfo(name).fileName().toUtf8()) > 0,
               "chain fixture writes identifiable contents");
    }
    auto chainExecution = makeBatchRenamePlan(
        {QUrl::fromLocalFile(chainFirst), QUrl::fromLocalFile(chainSecond)},
        QStringLiteral("4"), {}, false, 1, 2);
    verify(chainExecution.isValid() && batchRenamePlanReady(chainExecution),
           "test chain is safe before dispatch");
    for (int index : chainExecution.executionOrder) {
        const BatchRenameEntry &entry = chainExecution.entries.at(index);
        verify(batchRenameEntryReady(entry), "destination has been vacated before its move");
        verify(QFile::rename(entry.source.toLocalFile(), entry.destination.toLocalFile()),
               "test-only rename obeys dependency ordering");
    }
    verify(!QFileInfo::exists(chainFirst) && QFileInfo::exists(chainSecond)
               && QFileInfo::exists(chainFiles.filePath(QStringLiteral("441"))),
           "test chain reaches both distinct destinations without overwriting");
    QFile chainCheck(chainSecond);
    verify(chainCheck.open(QIODevice::ReadOnly) && chainCheck.readAll() == QByteArrayLiteral("1"),
           "the first item survives at its expected destination");
    QFile chainCheckNext(chainFiles.filePath(QStringLiteral("441")));
    verify(chainCheckNext.open(QIODevice::ReadOnly)
               && chainCheckNext.readAll() == QByteArrayLiteral("41"),
           "the second item survives without losing its contents");

    // The central Stage 1 fence must stop a real FileActions entry point
    // before it creates even the destination directory.
    const QString fenceJournal = processRecoveryRoot.filePath(QStringLiteral("cycle-v1.json"));
    QFile fenceFile(fenceJournal);
    const QByteArray fenceBytes = QByteArrayLiteral("{\"schema\":1}");
    verify(fenceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
               && fenceFile.write(fenceBytes) == fenceBytes.size(), "process fence v1 fixture");
    fenceFile.close();
    QWidget fenceHost;
    QString fenceMessage;
    int fenceDispatches = 0;
    FileActions fencedActions(&fenceHost, nullptr,
        [&fenceDispatches](KJob *, const QString &, bool, const QString &,
                           const FileActions::RefreshViews &) { ++fenceDispatches; },
        [&fenceMessage](const QString &message) { fenceMessage = message; });
    const QString refusedTarget = files.filePath(QStringLiteral("must-not-be-created"));
    fencedActions.copySelectionToDirectory({alpha}, refusedTarget, QStringLiteral("unused"));
    QFile unchangedFence(fenceJournal);
    verify(fenceDispatches == 0 && !QFileInfo::exists(refusedTarget)
               && !fenceMessage.isEmpty() && unchangedFence.open(QIODevice::ReadOnly)
               && unchangedFence.readAll() == fenceBytes,
           "v1 recovery fence blocks FileActions before mutation and preserves journal bytes");
    unchangedFence.close();
    verify(QFile::remove(fenceJournal), "test removes only its disposable fence fixture");
    BatchRenameRecoveryGate::instance().refresh();
    verify(!BatchRenameRecoveryGate::instance().mutationsBlocked(),
           "fence reopens after the test fixture is explicitly cleared");

    // A journal created while a modal prompt is open must be observed after
    // acceptance and before KIO constructs or starts any mutating job.
    const auto journalDuringInput = [&](const std::function<void()> &invoke,
                                        const QString &enteredName) {
        bool prompted = false;
        QTimer acceptInput;
        QObject::connect(&acceptInput, &QTimer::timeout, &app, [&] {
            auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!input) return;
            QFile late(fenceJournal);
            if (!late.exists()) {
                verify(late.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                           && late.write(fenceBytes) == fenceBytes.size(),
                       "late modal journal fixture is written in isolated recovery root");
                late.close();
            }
            input->setTextValue(enteredName);
            prompted = true;
            input->accept();
        });
        acceptInput.start(5);
        invoke();
        acceptInput.stop();
        verify(prompted, "modal mutation prompt was exercised");
        verify(QFile::remove(fenceJournal), "late modal journal fixture is explicitly removed");
        BatchRenameRecoveryGate::instance().refresh();
    };

    const int beforeRenameFence = fenceDispatches;
    const QString alphaOriginal = alpha.toLocalFile();
    const QString alphaBlockedName = files.filePath(QStringLiteral("blocked-rename.txt"));
    journalDuringInput([&] { fencedActions.renameSelected({alpha}, QStringLiteral("alpha.txt")); },
                       QStringLiteral("blocked-rename.txt"));
    verify(fenceDispatches == beforeRenameFence && QFileInfo::exists(alphaOriginal)
               && !QFileInfo::exists(alphaBlockedName),
           "journal appearing in rename dialog prevents dispatch and rename");

    const int beforeFileFence = fenceDispatches;
    const QString blockedFile = files.filePath(QStringLiteral("blocked-new.txt"));
    journalDuringInput([&] {
        fencedActions.createNewFile(QUrl::fromLocalFile(files.path()), {}, QByteArrayLiteral("x"));
    }, QStringLiteral("blocked-new.txt"));
    verify(fenceDispatches == beforeFileFence && !QFileInfo::exists(blockedFile),
           "journal appearing in new-file dialog prevents dispatch and creation");

    const int beforeFolderFence = fenceDispatches;
    const QString blockedFolder = files.filePath(QStringLiteral("blocked-folder"));
    journalDuringInput([&] {
        fencedActions.createNewFolder(QUrl::fromLocalFile(files.path()));
    }, QStringLiteral("blocked-folder"));
    verify(fenceDispatches == beforeFolderFence && !QFileInfo::exists(blockedFolder),
           "journal appearing in new-folder dialog prevents dispatch and creation");

    // Startup must expose a persistent non-modal fence, keep the event loop
    // responsive while the worker is deliberately slow, and disable mutating
    // actions for both a legacy pending journal and malformed v2 evidence.
    verify(writeJson(fenceJournal, QJsonObject{{QStringLiteral("schema"), 1}}),
           "startup pending-v1 fixture is isolated");
    qputenv("THISPC_RECOVERY_STARTUP_DELAY_MS", QByteArrayLiteral("250"));
    int responsiveTicks = 0;
    QTimer responsiveness;
    QObject::connect(&responsiveness, &QTimer::timeout, &app, [&] { ++responsiveTicks; });
    responsiveness.start(10);
    ThisPcWindow startupWindow(QUrl::fromLocalFile(files.path()), false);
    startupWindow.updateFileActionStates();
    verify(startupWindow.m_recoveryStatusLabel
               && !startupWindow.m_recoveryStatusLabel->isHidden()
               && startupWindow.m_recoveryStatusLabel->text().contains(
                   QStringLiteral("Checking"), Qt::CaseInsensitive)
               && !startupWindow.m_newFolderAction->isEnabled()
               && !startupWindow.m_pasteAction->isEnabled(),
           "startup checking banner is visible and mutations are disabled before worker completion");
    verify(QTest::qWaitFor([&] {
               return startupWindow.m_recoveryStartupWatcher
                   && startupWindow.m_recoveryStartupWatcher->isFinished();
           }, 5000) && responsiveTicks >= 5,
           "deliberately slow startup audit leaves the GUI event loop responsive");
    responsiveness.stop();
    qunsetenv("THISPC_RECOVERY_STARTUP_DELAY_MS");
    startupWindow.updateFileActionStates();
    verify(startupWindow.m_recoveryStatusLabel->text().contains(
               QStringLiteral("intervention"), Qt::CaseInsensitive)
               && !startupWindow.m_newFolderAction->isEnabled(),
           "completed startup scan reports pending v1 as requiring intervention");
    verify(QFile::remove(fenceJournal), "startup v1 fixture is explicitly removed");
    const QString invalidV2Path = processRecoveryRoot.filePath(QStringLiteral("cycle-invalid-v2.json"));
    QFile invalidV2(invalidV2Path);
    verify(invalidV2.open(QIODevice::WriteOnly | QIODevice::NewOnly)
               && invalidV2.write("{\"schema\":2") > 0,
           "startup invalid-v2 fixture is isolated");
    invalidV2.close();
    startupWindow.updateFileActionStates();
    verify(!startupWindow.m_recoveryStatusLabel->isHidden()
               && !startupWindow.m_newFolderAction->isEnabled(),
           "startup invalid v2 keeps persistent banner visible and mutations disabled");
    verify(QFile::remove(invalidV2Path), "startup invalid-v2 fixture is explicitly removed");
    startupWindow.updateFileActionStates();
    verify(startupWindow.m_recoveryStatusLabel->isHidden()
               && startupWindow.statusBar()->currentMessage().contains(
                   QStringLiteral("completed"), Qt::CaseInsensitive)
               && startupWindow.statusBar()->currentMessage().contains(
                   QStringLiteral("not restored"), Qt::CaseInsensitive)
               && startupWindow.m_newFolderAction->isEnabled(),
           "clean re-audit makes the truthful completion/history notice transient and restores mutations");
    qputenv("THISPC_RECOVERY_STARTUP_DELAY_MS", QByteArrayLiteral("150"));
    auto *closingWindow = new ThisPcWindow(QUrl::fromLocalFile(files.path()), false);
    verify(BatchRenameRecoveryGate::instance().startupScanPending()
               && closingWindow->m_recoveryStartupWatcher,
           "closing-window fixture starts a separate delayed startup scan");
    closingWindow->close();
    delete closingWindow;
    verify(QTest::qWaitFor([&] {
               return !BatchRenameRecoveryGate::instance().startupScanPending();
           }, 5000)
               && BatchRenameRecoveryGate::instance().ownsProtocolLock()
               && !BatchRenameRecoveryGate::instance().mutationsBlocked(),
           "closing a window during startup scan is safe and retains the process protocol lock");
    qunsetenv("THISPC_RECOVERY_STARTUP_DELAY_MS");

    // The batch runner yields between KIO jobs.  Evidence appearing in that
    // interval must stop the next asynchronous dispatch.
    const QUrl asyncOne = makeFile(QStringLiteral("async-one.txt"));
    const QUrl asyncTwo = makeFile(QStringLiteral("async-two.txt"));
    int asyncDispatches = 0;
    bool asyncJournalScheduled = false;
    FileActions asyncActions(&fenceHost, nullptr,
        [&](KJob *, const QString &, bool, const QString &,
            const FileActions::RefreshViews &) {
            ++asyncDispatches;
            if (!asyncJournalScheduled) {
                asyncJournalScheduled = true;
                QTimer::singleShot(0, &app, [&] {
                    QFile late(fenceJournal);
                    verify(late.open(QIODevice::WriteOnly | QIODevice::NewOnly)
                               && late.write(fenceBytes) == fenceBytes.size(),
                           "async continuation journal fixture is isolated");
                    late.close();
                });
            }
        });
    QTimer acceptBatch;
    QObject::connect(&acceptBatch, &QTimer::timeout, &app, [&] {
        auto *preview = dynamic_cast<BatchRenameDialog *>(QApplication::activeModalWidget());
        if (preview) {
            preview->m_prefix->setText(QStringLiteral("blocked-"));
            if (preview->m_buttons->button(QDialogButtonBox::Ok)->isEnabled())
                preview->m_buttons->button(QDialogButtonBox::Ok)->click();
        } else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            message->accept();
        }
    });
    acceptBatch.start(5);
    asyncActions.batchRenameSelected({asyncOne, asyncTwo});
    verify(QTest::qWaitFor([&] { return QFileInfo::exists(fenceJournal); }, 5000),
           "journal appears before asynchronous continuation");
    QTest::qWait(100);
    acceptBatch.stop();
    verify(asyncDispatches == 1
               && (QFileInfo::exists(asyncOne.toLocalFile())
                   || QFileInfo::exists(asyncTwo.toLocalFile())),
           "async callback recheck stops the second KIO dispatch");
    verify(QFile::remove(fenceJournal), "async continuation journal fixture is explicitly removed");
    BatchRenameRecoveryGate::instance().refresh();

    QProcess lockProbe;
    lockProbe.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    lockProbe.setProcessChannelMode(QProcess::ForwardedChannels);
    lockProbe.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("batch_rename"), QStringLiteral("--probe-recovery-lock"),
                     processRecoveryRoot.path()});
    const bool lockProbeFinished = lockProbe.waitForFinished(10000);
    if (!lockProbeFinished || lockProbe.exitCode() != 0)
        qWarning("lock probe: %s, exit=%d, error=%d", qPrintable(lockProbe.errorString()),
                 lockProbe.exitCode(), int(lockProbe.error()));
    verify(lockProbeFinished && lockProbe.exitStatus() == QProcess::NormalExit && lockProbe.exitCode() == 0,
           "separate process observes the kernel-held recovery inspector lock as live");

    // Stage 3C.2A: a pure pair is one reversible atomic syscall, NOT a
    // synthetic KIO command. Verify the actual UndoController routing and
    // refusal to touch a replaced inode using a disposable directory.
    QTemporaryDir historyFiles;
    verify(historyFiles.isValid(), "isolated single-swap history fixture");
    const QString historyLeft = historyFiles.filePath(QStringLiteral("uv"));
    const QString historyRight = historyFiles.filePath(QStringLiteral("vu"));
    for (const QString &name : {historyLeft, historyRight}) {
        QFile fixture(name);
        verify(fixture.open(QIODevice::WriteOnly)
                   && fixture.write(QFileInfo(name).fileName().toUtf8()) == 2,
               "atomic history fixture writes distinct content");
    }
    auto historyPlan = makeBatchRenamePlan(
        {QUrl::fromLocalFile(historyLeft), QUrl::fromLocalFile(historyRight)}, cycleOptions);
    verify(historyPlan.isValid() && historyPlan.atomicSwaps.size() == 1,
           "pure pair is eligible for atomic history");
    QString undoStatus;
    UndoController history(nullptr, [](bool) {}, [] {},
                           [&undoStatus](const QString &message, int) { undoStatus = message; });
    QAction undoAction;
    QAction redoAction;
    history.setActions(&undoAction, &redoAction);
    history.invalidateUndoBeforeAtomicSwap();
    const auto historyPair = historyPlan.atomicSwaps.first();
    verify(batchRenameAtomicSwap(historyPlan, historyPair.first, historyPair.second),
           "original operation uses one atomic exchange");
    verify(history.recordCompletedAtomicSwap(historyPlan) && undoAction.isEnabled(),
           "successful pure pair is recorded as a single Undo action");
    verify(!history.recordCompletedAtomicSwap(chainExecution),
           "a chain must not be misrepresented as atomic swap history");
    history.undo();
    verify(!undoAction.isEnabled() && !redoAction.isEnabled()
               && BatchRenameRecoveryGate::instance().mutationsBlocked(),
           "asynchronous swap Undo immediately disables history and all mutations");
    verify(QTest::qWaitFor([&] {
        return redoAction.isEnabled()
            && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "swap Undo worker completes before Redo becomes available");
    QFile checkUndoLeft(historyLeft);
    QFile checkUndoRight(historyRight);
    verify(checkUndoLeft.open(QIODevice::ReadOnly) && checkUndoLeft.readAll() == "uv"
               && checkUndoRight.open(QIODevice::ReadOnly) && checkUndoRight.readAll() == "vu"
               && redoAction.isEnabled(),
           "one Undo restores both names and enables Redo");
    checkUndoLeft.close();
    checkUndoRight.close();
    history.redo();
    verify(QTest::qWaitFor([&] {
        return undoAction.isEnabled()
            && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "swap Redo worker completes before Undo becomes available");
    QFile checkRedoLeft(historyLeft);
    QFile checkRedoRight(historyRight);
    verify(checkRedoLeft.open(QIODevice::ReadOnly) && checkRedoLeft.readAll() == "vu"
               && checkRedoRight.open(QIODevice::ReadOnly) && checkRedoRight.readAll() == "uv"
               && undoAction.isEnabled(),
           "one Redo repeats the exchange without losing either payload");
    checkRedoLeft.close();
    checkRedoRight.close();
    history.undo();
    verify(QTest::qWaitFor([&] {
        return redoAction.isEnabled()
            && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "swap worker finishes before a replacement-inode conflict is introduced");
    const QString savedHistoryLeft = historyFiles.filePath(QStringLiteral("saved-uv"));
    verify(QFile::rename(historyLeft, savedHistoryLeft),
           "test fixture moves recorded inode out of its original name");
    QFile historyReplacement(historyLeft);
    verify(historyReplacement.open(QIODevice::WriteOnly)
               && historyReplacement.write("outsider") == 8,
           "test fixture replaces an expected inode after Undo");
    historyReplacement.close();
    history.redo();
    verify(QTest::qWaitFor([&] {
        return !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "refused asynchronous swap Redo completes without hanging");
    QFile checkRefusedRight(historyRight);
    QFile checkRefusedLeft(historyLeft);
    verify(checkRefusedRight.open(QIODevice::ReadOnly)
               && checkRefusedRight.readAll() == "vu"
               && checkRefusedLeft.open(QIODevice::ReadOnly)
               && checkRefusedLeft.readAll() == "outsider"
               && !undoStatus.isEmpty(),
           "Redo refuses a replaced inode without exchanging either directory entry");
    checkRefusedRight.close();
    checkRefusedLeft.close();

    // Stage 4A.2: exercise the actual FileActions preview -> confirmation ->
    // async swap worker, with an injected worker delay. No v2 swap journal is
    // generated, and the production recovery allowlist remains linear-only.
    QTemporaryDir asyncSwapFiles;
    verify(asyncSwapFiles.isValid(), "isolated FileActions swap fixture");
    const QString asyncLeft = asyncSwapFiles.filePath(QStringLiteral("ab"));
    const QString asyncRight = asyncSwapFiles.filePath(QStringLiteral("ba"));
    for (const QString &name : {asyncLeft, asyncRight}) {
        QFile fixture(name);
        verify(fixture.open(QIODevice::WriteOnly)
                   && fixture.write(QFileInfo(name).fileName().toUtf8()) == 2,
               "FileActions swap fixture writes separate payloads");
    }
    const QList<QUrl> asyncSources{QUrl::fromLocalFile(asyncLeft),
                                  QUrl::fromLocalFile(asyncRight)};
    const auto asyncSwapPlan = makeBatchRenamePlan(asyncSources, cycleOptions);
    verify(asyncSwapPlan.isValid() && asyncSwapPlan.atomicSwaps.size() == 1,
           "async integration fixture has one exchange with recorded dev/inode");
    QFile untouched(asyncSwapFiles.filePath(QStringLiteral("untouched")));
    verify(untouched.open(QIODevice::WriteOnly)
               && untouched.write("SAFE") == 4,
           "unrelated entry for in-flight mutation fence");
    untouched.close();
    QWidget asyncSwapHost;
    asyncSwapHost.show();
    UndoController asyncSwapHistory(&asyncSwapHost, [](bool) {}, [] {},
                                    [](const QString &, int) {}, &asyncSwapHost);
    QAction asyncSwapUndo;
    QAction asyncSwapRedo;
    asyncSwapHistory.setActions(&asyncSwapUndo, &asyncSwapRedo);
    int asyncSwapKioJobs = 0;
    FileActions asyncSwapActions(&asyncSwapHost, &asyncSwapHistory,
        [&asyncSwapKioJobs](KJob *, const QString &, bool, const QString &,
                            const FileActions::RefreshViews &) { ++asyncSwapKioJobs; });
    int asyncPreviewCount = 0;
    int asyncConfirmationCount = 0;
    QTimer acceptSwap;
    QObject::connect(&acceptSwap, &QTimer::timeout, &app, [&] {
        if (auto *preview = dynamic_cast<BatchRenameDialog *>(QApplication::activeModalWidget())) {
            preview->m_regex->setChecked(true);
            preview->m_find->setText(QStringLiteral("^(.)(.)$"));
            preview->m_replacement->setText(QStringLiteral("\\2\\1"));
            if (preview->m_buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
                ++asyncPreviewCount;
                preview->m_buttons->button(QDialogButtonBox::Ok)->click();
            }
        } else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (asyncConfirmationCount) return;
            if (auto *yes = message->button(QMessageBox::Yes)) {
                ++asyncConfirmationCount;
                yes->click();
            }
        }
    });
    qputenv("THISPC_SWAP_WORKER_DELAY_MS", "300");
    int guiTicks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&] { ++guiTicks; });
    heartbeat.start(10);
    acceptSwap.start(5);

    asyncSwapActions.batchRenameSelected(asyncSources);

    acceptSwap.stop();

    verify(asyncPreviewCount == 1 && asyncConfirmationCount == 1,
           "real swap is accepted in preview and confirmation");
    // Do not count timer ticks from the nested preview/confirmation dialogs.
    guiTicks = 0;
    verify(BatchRenameRecoveryGate::instance().unjournaledSwapRunning()
               && !asyncSwapUndo.isEnabled() && !asyncSwapRedo.isEnabled(),
           "swap worker holds global mutation fence before GUI resumes");
    asyncSwapActions.renameSelected({QUrl::fromLocalFile(untouched.fileName())},
                                    QStringLiteral("untouched"));
    verify(QFileInfo::exists(untouched.fileName())
               && !QFileInfo::exists(asyncSwapFiles.filePath(QStringLiteral("renamed")))
               && asyncSwapKioJobs == 0,
           "a competing FileActions mutation cannot dispatch while swap runs");
    verify(QTest::qWaitFor([&] {
        return asyncSwapUndo.isEnabled()
        && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "actual FileActions swap finishes and records exactly one Undo");


    heartbeat.stop();
    qunsetenv("THISPC_SWAP_WORKER_DELAY_MS");
    verify(guiTicks >= 3, "GUI event loop remains responsive during delayed swap worker");
    QFile actualSwapLeft(asyncLeft);
    QFile actualSwapRight(asyncRight);
    verify(actualSwapLeft.open(QIODevice::ReadOnly)
               && actualSwapLeft.readAll() == QByteArrayLiteral("ba")
               && actualSwapRight.open(QIODevice::ReadOnly)
               && actualSwapRight.readAll() == QByteArrayLiteral("ab")
               && batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(0), asyncSources.at(1))
               && batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(1), asyncSources.at(0))
               && asyncSwapKioJobs == 0,
           "actual async swap preserves payloads/inodes and never dispatches fake KIO jobs");
    actualSwapLeft.close();
    actualSwapRight.close();

    asyncSwapHistory.undo();
    verify(QTest::qWaitFor([&] {
        return asyncSwapRedo.isEnabled()
            && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "actual async swap has single-step Undo");
    QFile checkAsyncUndoLeft(asyncLeft);
    QFile checkAsyncUndoRight(asyncRight);
    verify(checkAsyncUndoLeft.open(QIODevice::ReadOnly)
               && checkAsyncUndoLeft.readAll() == QByteArrayLiteral("ab")
               && checkAsyncUndoRight.open(QIODevice::ReadOnly)
               && checkAsyncUndoRight.readAll() == QByteArrayLiteral("ba")
               && batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(0), asyncSources.at(0))
               && batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(1), asyncSources.at(1)),
           "actual async swap Undo restores both payloads and inodes");
    checkAsyncUndoLeft.close();
    checkAsyncUndoRight.close();
    asyncSwapHistory.redo();
    verify(QTest::qWaitFor([&] {
        return asyncSwapUndo.isEnabled()
            && !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "actual async swap has single-step Redo");
    verify(batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(0), asyncSources.at(1))
               && batchRenamePathHasOriginalIdentity(asyncSwapPlan.entries.at(1), asyncSources.at(0))
               && BatchRenameRecoveryJournal::pendingJournal().isEmpty(),
           "actual async swap Redo preserves inode mappings and creates no v2 journal");

    // Stage 4A.2 audit: two independent pairs form ONE active batch, even
    // between worker completions. A delayed continuation exposes the former
    // gap without opening a modal error or touching files outside fixtures.
    QTemporaryDir gapSwapFiles;
    verify(gapSwapFiles.isValid(), "isolated two-swap batch fixture");
    QList<QUrl> gapSources;
    for (const QString &name : {QStringLiteral("ab"), QStringLiteral("ba"),
                                QStringLiteral("cd"), QStringLiteral("dc")}) {
        const QString path = gapSwapFiles.filePath(name);
        QFile fixture(path);
        const QByteArray bytes = name.toUtf8();
        verify(fixture.open(QIODevice::WriteOnly) && fixture.write(bytes) == bytes.size(),
               "two-swap fixture has distinct payloads");
        gapSources.append(QUrl::fromLocalFile(path));
    }
    const auto gapPlan = makeBatchRenamePlan(gapSources, cycleOptions);
    verify(gapPlan.isValid() && gapPlan.executionOrder.isEmpty()
               && gapPlan.exchangeCycles.isEmpty() && gapPlan.atomicSwaps.size() == 2,
           "two isolated exchanges share one Batch Rename plan");
    const auto firstGapPair = gapPlan.atomicSwaps.at(0);
    const auto secondGapPair = gapPlan.atomicSwaps.at(1);
    const auto pairExchanged = [&](const QPair<int, int> &pair) {
        return batchRenamePathHasOriginalIdentity(gapPlan.entries.at(pair.first),
                                                  gapPlan.entries.at(pair.second).source)
            && batchRenamePathHasOriginalIdentity(gapPlan.entries.at(pair.second),
                                                  gapPlan.entries.at(pair.first).source);
    };
    const auto pairOriginal = [&](const QPair<int, int> &pair) {
        return batchRenamePathHasOriginalIdentity(gapPlan.entries.at(pair.first),
                                                  gapPlan.entries.at(pair.first).source)
            && batchRenamePathHasOriginalIdentity(gapPlan.entries.at(pair.second),
                                                  gapPlan.entries.at(pair.second).source);
    };
    asyncPreviewCount = 0;
    asyncConfirmationCount = 0;
    qputenv("THISPC_SWAP_BATCH_GAP_DELAY_MS", "1200");
    acceptSwap.start(5);
    asyncSwapActions.batchRenameSelected(gapSources);
    acceptSwap.stop();
    verify(asyncPreviewCount == 1 && asyncConfirmationCount == 1,
           "two-swap batch preview and confirmation are accepted");
    verify(QTest::qWaitFor([&] {
        return pairExchanged(firstGapPair) && !asyncSwapActions.m_swapWorkerRunning;
    }, 10000), "first worker finished before the intentionally delayed continuation");
    verify(pairOriginal(secondGapPair) && asyncSwapActions.m_swapBatchRunning
               && asyncSwapHistory.m_groupedBatchRunning
               && BatchRenameRecoveryGate::instance().unjournaledSwapRunning()
               && BatchRenameRecoveryGate::instance().mutationsBlocked()
               && !asyncSwapUndo.isEnabled() && !asyncSwapRedo.isEnabled(),
           "gap keeps global mutation fence and grouped Undo/Redo disabled");
    QCloseEvent gapClose;
    QApplication::sendEvent(&startupWindow, &gapClose);
    verify(!gapClose.isAccepted(),
           "main-window close is rejected in the gap between two exchanges");
    const int gapJobs = asyncSwapKioJobs;
    asyncSwapActions.renameSelected({QUrl::fromLocalFile(untouched.fileName())},
                                    QStringLiteral("untouched"));
    verify(asyncSwapKioJobs == gapJobs && QFileInfo::exists(untouched.fileName())
               && pairOriginal(secondGapPair),
           "competing mutation cannot dispatch in the gap between two exchanges");
    qunsetenv("THISPC_SWAP_BATCH_GAP_DELAY_MS");
    verify(QTest::qWaitFor([&] {
        return !BatchRenameRecoveryGate::instance().unjournaledSwapRunning();
    }, 10000), "whole-batch fence releases after the second exchange and terminal cleanup");
    verify(pairExchanged(firstGapPair) && pairExchanged(secondGapPair)
               && !asyncSwapActions.m_swapBatchRunning
               && !asyncSwapHistory.m_groupedBatchRunning
               && !BatchRenameRecoveryGate::instance().mutationsBlocked()
               && !asyncSwapUndo.isEnabled() && !asyncSwapRedo.isEnabled()
               && BatchRenameRecoveryJournal::pendingJournal().isEmpty(),
           "both exchanges complete with verified inodes, no false grouped Undo or swap journal");

    // Real UndoController dispatch is asynchronous: fsync and exchanges run
    // outside the GUI thread, and the finished callback enables next action.
    QString cycleStatus;
    UndoController cycleHistory(nullptr, [](bool) {}, [] {},
                                [&cycleStatus](const QString &message, int) {
                                    cycleStatus = message;
                                });
    QAction cycleUndoAction;
    QAction cycleRedoAction;
    cycleHistory.setActions(&cycleUndoAction, &cycleRedoAction);
    cycleHistory.invalidateUndoBeforeAtomicSwap();
    verify(cycleHistory.recordCompletedCycle(triple) && cycleUndoAction.isEnabled()
               && !cycleRedoAction.isEnabled()
               && !cycleHistory.recordCompletedCycle(chainExecution),
           "only an isolated complete cycle becomes one Undo action, never a chain");
    cycleHistory.undo();
    for (int attempt = 0; attempt < 1000 && !cycleRedoAction.isEnabled(); ++attempt)
        QTest::qWait(10);
    verify(cycleRedoAction.isEnabled() && !cycleUndoAction.isEnabled()
               && batchRenameCycleReady(triple, tripleCycle)
               && BatchRenameRecoveryJournal::pendingJournal().isEmpty(),
           "one Ctrl+Z routes to the worker and undoes the entire three-cycle");
    cycleHistory.redo();
    for (int attempt = 0; attempt < 1000 && !cycleUndoAction.isEnabled(); ++attempt)
        QTest::qWait(10);
    verify(cycleUndoAction.isEnabled() && !cycleRedoAction.isEnabled()
               && batchRenameCycleSnapshotsMatch(triple, tripleCycle,
                                                 batchRenameCycleFinalState(tripleCycle))
               && BatchRenameRecoveryJournal::pendingJournal().isEmpty(),
           "one Ctrl+Y redoes the entire three-cycle through journaled worker");
    verify(!cycleHistory.recordCompletedCycle(historyPlan),
           "the history API never confuses a two-way swap with a multi-step cycle");
    // A stale path with an outsider inode MUST NOT become an Undo target.
    const QString heldCycleFile = abc.toLocalFile() + QStringLiteral(".held");
    verify(QFile::rename(abc.toLocalFile(), heldCycleFile),
           "test moves one cycle inode aside before a replay");
    QFile outsiderCycle(abc.toLocalFile());
    verify(outsiderCycle.open(QIODevice::WriteOnly)
               && outsiderCycle.write("OUTSIDER") == 8,
           "external file takes the former cycle pathname");
    outsiderCycle.close();
    const auto staleCycleUndo = batchRenameReplaySingleCycle(triple, true, replayRoot.path());
    QFile outsiderCheck(abc.toLocalFile());
    verify(!staleCycleUndo.success && !staleCycleUndo.uncertain
               && BatchRenameRecoveryJournal::pendingJournal(replayRoot.path()).isEmpty()
               && outsiderCheck.open(QIODevice::ReadOnly)
               && outsiderCheck.readAll() == QByteArrayLiteral("OUTSIDER")
               && QFileInfo::exists(heldCycleFile),
           "stale inode refuses whole-cycle Undo without touching the outsider");
    outsiderCheck.close();

    // Stage 3C.2B.2A: two independent local vacancy-first chains are ONE
    // user-visible Undo/Redo, but their replay is non-atomic and journaled.
    QTemporaryDir linearFiles;
    QTemporaryDir linearRecovery;
    verify(linearFiles.isValid() && linearRecovery.isValid(),
           "isolated linear history and journal fixtures");
    QList<QUrl> linearSources;
    for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                QStringLiteral("2"), QStringLiteral("42")}) {
        const QString path = linearFiles.filePath(name);
        QFile item(path);
        verify(item.open(QIODevice::WriteOnly) && item.write(name.toUtf8()) == name.toUtf8().size(),
               "independent chain fixture writes identifiable content");
        item.close();
        linearSources.push_back(QUrl::fromLocalFile(path));
    }
    const auto linearPlan = makeBatchRenamePlan(linearSources, QStringLiteral("4"),
                                                {}, false, 1, 2);
    verify(batchRenameLinearHistoryEligible(linearPlan)
               && linearPlan.executionOrder.size() == 4
               && batchRenameLinearMappingMatches(linearPlan, false, 0),
           "two chains qualify for one guarded history entry");
    verify(!batchRenameLinearHistoryEligible(triple)
               && !batchRenameLinearHistoryEligible(cycle),
           "cycles and swaps cannot be replayed as linear history");
    // The test uses the same strict Linux primitive as history Redo; normal
    // application execution remains asynchronous KIO with per-step Undo until complete.
    const auto initialLinear = batchRenameReplayLinear(linearPlan, false, linearRecovery.path());
    verify(initialLinear.success && !initialLinear.uncertain
               && batchRenameLinearMappingMatches(linearPlan, true, 0)
               && BatchRenameRecoveryJournal::pendingJournal(linearRecovery.path()).isEmpty(),
           "linear worker runs both chains and closes its journal");
    QString linearStatus;
    int linearStatusTimeout = 0;
    bool linearRefreshPreservedStatus = false;
    UndoController linearHistory(nullptr,
        [&linearRefreshPreservedStatus](bool preserve) {
            linearRefreshPreservedStatus = preserve;
        }, [] {},
        [&linearStatus, &linearStatusTimeout](const QString &text, int timeout) {
            linearStatus = text;
            linearStatusTimeout = timeout;
        });
    QAction linearUndo;
    QAction linearRedo;
    linearHistory.setActions(&linearUndo, &linearRedo);
    verify(linearHistory.recordCompletedLinear(linearPlan) && linearUndo.isEnabled(),
           "completed multi-chain batch is registered as one Undo");
    linearHistory.undo();
    for (int attempt = 0; attempt < 1000 && !linearRedo.isEnabled(); ++attempt)
        QTest::qWait(10);
    verify(linearRedo.isEnabled() && !linearUndo.isEnabled()
               && batchRenameLinearMappingMatches(linearPlan, false, 0),
           "one Ctrl+Z restores the two complete chains");
    linearHistory.redo();
    for (int attempt = 0; attempt < 1000 && !linearUndo.isEnabled(); ++attempt)
        QTest::qWait(10);
    verify(linearUndo.isEnabled() && !linearRedo.isEnabled()
               && batchRenameLinearMappingMatches(linearPlan, true, 0),
           "one Ctrl+Y reapplies both chains without overwriting anything");
    for (const QString &name : {QStringLiteral("41"), QStringLiteral("441"),
                                QStringLiteral("42"), QStringLiteral("442")}) {
        const QString original = name.mid(1);
        QFile result(linearFiles.filePath(name));
        verify(result.open(QIODevice::ReadOnly) && result.readAll() == original.toUtf8(),
               "every destination retained its original payload through Undo/Redo");
    }
    linearHistory.undo();
    verify(QTest::qWaitFor([&] {
        return linearRedo.isEnabled() && batchRenameLinearMappingMatches(linearPlan, false, 0);
    }, 10000), "controller Undo prepares the guarded Redo regression");
    const QString replacedLinearPath = linearFiles.filePath(QStringLiteral("1"));
    const QString savedLinearPath = linearFiles.filePath(QStringLiteral(".original-1-backup"));
    verify(QFile::rename(replacedLinearPath, savedLinearPath),
           "test keeps the expected inode under a safe backup name");
    QFile replacementLinear(replacedLinearPath);
    verify(replacementLinear.open(QIODevice::WriteOnly)
               && replacementLinear.write("REPLACEMENT") == 11,
           "test substitutes a different inode before Redo");
    replacementLinear.close();
    QHash<QString, QByteArray> beforeRefusedRedo;
    for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                QStringLiteral("2"), QStringLiteral("42"),
                                QStringLiteral(".original-1-backup")}) {
        QFile file(linearFiles.filePath(name));
        verify(file.open(QIODevice::ReadOnly), "refusal snapshot opens every directory entry");
        beforeRefusedRedo.insert(name, file.readAll());
    }
    linearRefreshPreservedStatus = false;
    linearHistory.redo();
    verify(QTest::qWaitFor([&] { return !linearHistory.m_busy; }, 10000),
           "replaced-inode Redo refusal completes asynchronously");
    bool refusedRedoUnchanged = true;
    for (auto it = beforeRefusedRedo.cbegin(); it != beforeRefusedRedo.cend(); ++it) {
        QFile file(linearFiles.filePath(it.key()));
        refusedRedoUnchanged = refusedRedoUnchanged && file.open(QIODevice::ReadOnly)
            && file.readAll() == it.value();
    }
    verify(refusedRedoUnchanged && linearRedo.isEnabled() && !linearUndo.isEnabled(),
           "refused Redo changes no entry and remains available for a safe retry");
    verify(linearStatusTimeout == 12000
               && (linearStatus.contains(QStringLiteral("inode"), Qt::CaseInsensitive))
               && (linearStatus.contains(QStringLiteral("ponowi"), Qt::CaseInsensitive)
                   || linearStatus.contains(QStringLiteral("try again"), Qt::CaseInsensitive))
               && linearRefreshPreservedStatus,
           "refusal explains inode change and marks its async refresh to preserve status");
    verify(QFile::remove(replacedLinearPath) && QFile::rename(savedLinearPath, replacedLinearPath),
           "test safely restores the expected inode for retry");
    linearHistory.redo();
    verify(QTest::qWaitFor([&] {
        return linearUndo.isEnabled() && batchRenameLinearMappingMatches(linearPlan, true, 0);
    }, 10000), "Redo succeeds when the expected files are safely restored");
    QString linearJournalError;
    {
        BatchRenameRecoveryJournal interruptedLinear(linearRecovery.path());
        verify(interruptedLinear.beginLinear(linearPlan, true, &linearJournalError)
                   && interruptedLinear.checkpointLinear(QStringLiteral("undo-intent"), 0,
                                                         &linearJournalError),
               "linear history persists intent before moving any entry");
    }
    const QString unresolvedLinear =
        BatchRenameRecoveryJournal::pendingJournal(linearRecovery.path());
    verify(!unresolvedLinear.isEmpty()
               && QFileInfo(unresolvedLinear).fileName().startsWith(QStringLiteral("linear-")),
           "interrupted linear journal remains discoverable after object destruction");
    {
        BatchRenameRecoveryJournal blockedLinear(linearRecovery.path());
        verify(!blockedLinear.beginLinear(linearPlan, true, &linearJournalError)
                   && QFileInfo::exists(unresolvedLinear),
               "an unresolved linear manifest fences subsequent history operations");
    }
    verify(QFile::remove(unresolvedLinear),
           "isolated test clears its own untouched, verified journal fixture");
    const auto injectedLinear = batchRenameReplayLinear(linearPlan, true,
                                                         linearRecovery.path(), 1);
    verify(!injectedLinear.success && !injectedLinear.uncertain
               && batchRenameLinearMappingMatches(linearPlan, true, 0)
               && BatchRenameRecoveryJournal::pendingJournal(linearRecovery.path()).isEmpty(),
           "injected mid-Undo failure rolls back the verified prefix and closes journal");
    const QString outsiderPath = linearFiles.filePath(QStringLiteral("1"));
    QFile outsiderLinear(outsiderPath);
    verify(outsiderLinear.open(QIODevice::WriteOnly)
               && outsiderLinear.write("OUTSIDER") == 8,
           "outsider occupies the previously vacant chain source");
    outsiderLinear.close();
    const auto refusedLinear = batchRenameReplayLinear(linearPlan, true, linearRecovery.path());
    QFile outsiderProof(outsiderPath);
    verify(!refusedLinear.success && !refusedLinear.uncertain
               && outsiderProof.open(QIODevice::ReadOnly)
               && outsiderProof.readAll() == QByteArrayLiteral("OUTSIDER")
               && BatchRenameRecoveryJournal::pendingJournal(linearRecovery.path()).isEmpty(),
           "outsider in an original vacancy rejects the whole Undo before the first move");
    outsiderProof.close();

    // Real FileActions integration regression for the reported KDE bug:
    // running the batch through its actual preview and confirmation must NOT
    // enqueue four per-item KIO Undo entries ahead of the grouped record.
    QTemporaryDir integratedFiles;
    verify(integratedFiles.isValid(), "integration batch fixture is isolated");
    QList<QUrl> integratedSources;
    for (const QString &name : {QStringLiteral("1"), QStringLiteral("41"),
                                QStringLiteral("2"), QStringLiteral("42")}) {
        QFile file(integratedFiles.filePath(name));
        verify(file.open(QIODevice::WriteOnly) && file.write(name.toUtf8()) == name.toUtf8().size(),
               "real FileActions fixture contains identifiable data");
        file.close();
        integratedSources.push_back(QUrl::fromLocalFile(file.fileName()));
    }
    const auto integratedPlan = makeBatchRenamePlan(integratedSources, QStringLiteral("4"),
                                                    {}, false, 1, 2);
    verify(batchRenameLinearHistoryEligible(integratedPlan),
           "real dispatch fixture contains two independent vacancy-first chains");
    QWidget integratedHost;
    integratedHost.show();
    UndoController integratedHistory(&integratedHost, [](bool) {}, [] {},
        [](const QString &, int) {}, &integratedHost);
    QAction integratedUndo;
    QAction integratedRedo;
    integratedHistory.setActions(&integratedUndo, &integratedRedo);
    int dispatchedKioJobs = 0;
    FileActions integratedActions(&integratedHost, &integratedHistory,
        [&dispatchedKioJobs](KJob *, const QString &, bool, const QString &,
                             const FileActions::RefreshViews &) { ++dispatchedKioJobs; });
    int previewCount = 0;
    int confirmationCount = 0;
    bool confirmationClicked = false;
    QTimer autoDialog;
    QObject::connect(&autoDialog, &QTimer::timeout, &app, [&] {
        if (auto *preview = dynamic_cast<BatchRenameDialog *>(QApplication::activeModalWidget())) {
            preview->m_prefix->setText(QStringLiteral("4"));
            if (preview->m_buttons->button(QDialogButtonBox::Ok)->isEnabled()) {
                ++previewCount;
                preview->m_buttons->button(QDialogButtonBox::Ok)->click();
            }
        } else if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (confirmationClicked) return;
            auto *yes = message->button(QMessageBox::Yes);
            if (!yes) return;
            confirmationClicked = true;
            ++confirmationCount;
            yes->click();
        }
    });
    autoDialog.start(5);
    integratedActions.batchRenameSelected(integratedSources);
    autoDialog.stop();
    verify(previewCount == 1 && confirmationCount == 1,
           "real batch passes through preview and grouped-history confirmation");
    const bool groupedReady = QTest::qWaitFor([&] {
        return batchRenameLinearMappingMatches(integratedPlan, true, 0)
        && integratedUndo.isEnabled()
        && (integratedUndo.toolTip().contains(QStringLiteral("partii"))
        || integratedUndo.toolTip().contains(QStringLiteral("batch")));
    }, 10000);

    verify(groupedReady,
           "real batch registers one grouped Undo after all moves finish");
    verify(dispatchedKioJobs == 0,
           "eligible initial batch never dispatches individually recorded KIO moves");
    integratedHistory.undo();
    verify(QTest::qWaitFor([&] {
        return batchRenameLinearMappingMatches(integratedPlan, false, 0)
            && integratedRedo.isEnabled();
    }, 10000), "one actual FileActions Undo restores all four sources");
    integratedHistory.redo();
    verify(QTest::qWaitFor([&] {
        return batchRenameLinearMappingMatches(integratedPlan, true, 0)
            && integratedUndo.isEnabled();
    }, 10000), "one actual FileActions Redo replays both chains");
    for (const QString &name : {QStringLiteral("41"), QStringLiteral("441"),
                                QStringLiteral("42"), QStringLiteral("442")}) {
        QFile file(integratedFiles.filePath(name));
        verify(file.open(QIODevice::ReadOnly) && file.readAll() == name.mid(1).toUtf8(),
               "FileActions Undo/Redo preserves every source payload");
    }

    BatchRenameDialog dialog({alpha, beta});
    verify(dialog.findChild<QTableWidget *>() != nullptr,
           "preview dialog exposes the old-to-new table");
    verify(dialog.m_prefix != nullptr && dialog.m_suffix != nullptr,
           "preview dialog exposes prefix and suffix fields");
    verify(dialog.m_find != nullptr && dialog.m_replacement != nullptr && dialog.m_regex != nullptr
               && dialog.m_case != nullptr && dialog.m_extensionMode != nullptr,
           "Stage 2 controls are present in the preview dialog");
    BatchRenameDialog mixedDialog({readme, mixedDotfile, mixedFolder,
                                   QUrl::fromLocalFile(activeRemoval.destination.toLocalFile())});
    mixedDialog.m_extensionMode->setCurrentIndex(
        static_cast<int>(BatchRenameExtension::Remove));
    verify(mixedDialog.m_plan.isValid() && mixedDialog.m_plan.activeCount() == 1
               && mixedDialog.m_buttons->button(QDialogButtonBox::Ok)->isEnabled(),
           "preview enables Rename for a real change mixed with skipped rows");
    BatchRenameDialog conflictDialog({one, two});
    conflictDialog.m_prefix->setText(QStringLiteral("4"));
    verify(conflictDialog.m_table->rowCount() == 2
               && conflictDialog.m_table->currentRow() == 0
               && !conflictDialog.m_buttons->button(QDialogButtonBox::Ok)->isEnabled(),
           "invalid screenshot preview remains populated, selects its row, and blocks apply");
    qInfo("Batch Rename: %d assertions PASS", checks);
}
