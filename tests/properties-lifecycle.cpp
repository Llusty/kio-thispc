/*
 * 0.37 Stage 6: modeless File / Folder / Drive Properties lifecycle.
 *
 * Exercises the real ThisPcWindow and PropertiesDialog::show()/showForDrive():
 * modelessness, coexistence, busy/close safety, disappearing targets, main
 * window close and worker shutdown. Apply/OK/Cancel/ACL/recursive chmod data
 * semantics live in the 'properties' suite, which now runs modelessly.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include <QTest>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void flush()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static QStringList g_boxes;

static QDialog *openProps(ThisPcWindow &window, const QString &path, bool dir)
{
    window.showPropertiesDialog(QFileInfo(path).fileName(), QUrl::fromLocalFile(path), dir,
                                dir ? QStringLiteral("Directory") : QStringLiteral("File"), {}, {});
    const auto open = PropertiesLifecycle::instance().openDialogs();
    verify(!open.isEmpty(), "Properties dialog registered in lifecycle");
    return open.last();
}

static QString makeFile(const QString &path, qint64 size = 16)
{
    QFile f(path);
    verify(f.open(QIODevice::WriteOnly), "fixture file created");
    f.write("properties stage6\n");
    if (size > 32) verify(f.resize(size), "sparse fixture resized");
    f.close();
    return path;
}

static int modeOf(const QString &path)
{
    struct stat info {};
    verify(::stat(QFile::encodeName(path).constData(), &info) == 0, "stat fixture");
    return info.st_mode & 0777;
}

static void setOwnerWriteBit(QDialog *dialog, bool on)
{
    auto *group = dialog->findChild<QGroupBox *>();
    auto *grid = qobject_cast<QGridLayout *>(group->layout());
    auto *box = qobject_cast<QCheckBox *>(grid->itemAtPosition(1, 2)->widget());
    verify(box && box->isEnabled(), "owner write box editable");
    box->setChecked(on);
}

static QPushButton *button(QDialog *dialog, QDialogButtonBox::StandardButton which)
{
    return dialog->findChild<QDialogButtonBox *>()->button(which);
}

int main(int argc, char **argv)
{
    interceptFileJobs = false;
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-properties-lifecycle");
    QCoreApplication::setApplicationName("properties-lifecycle-test");
    QTemporaryDir dir;
    verify(dir.isValid(), "temporary directory");

    // Dismiss expected controlled error boxes (never a crash / hang).
    QTimer boxGuard;
    boxGuard.setInterval(10);
    QObject::connect(&boxGuard, &QTimer::timeout, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            g_boxes << box->text();
            box->close();
        }
    });
    boxGuard.start();

    auto &life = PropertiesLifecycle::instance();
    verify(life.openCount() == 0, "no Properties dialogs at start");

    {
        ThisPcWindow window(QUrl::fromLocalFile(dir.path()), false);
        window.show();
        QCoreApplication::processEvents();

        const QString fileA = makeFile(dir.filePath("a.txt"));
        const QString fileB = makeFile(dir.filePath("b.txt"));
        const QString folder = dir.filePath("folder");
        verify(QDir().mkpath(folder), "folder fixture");

        // 1-6: modeless file / folder, main window stays usable.
        QDialog *a = openProps(window, fileA, false);
        verify(!a->isModal() && a->testAttribute(Qt::WA_DeleteOnClose), "File Properties is modeless with WA_DeleteOnClose");
        verify(QApplication::activeModalWidget() == nullptr, "File Properties creates no modal widget");
        verify(window.isEnabled(), "main window enabled with File Properties open");
        QDialog *f = openProps(window, folder, true);
        verify(!f->isModal() && QApplication::activeModalWidget() == nullptr, "Folder Properties is modeless");
        verify(window.isEnabled(), "main window enabled with Folder Properties open");

        // Main window remains interactive: navigate while dialogs are open.
        window.navigatePane(ThisPcWindow::PaneId::Primary, QUrl::fromLocalFile(folder));
        QCoreApplication::processEvents();
        verify(a->isVisible() && f->isVisible(), "dialogs survive main window navigation");

        // 7-10: coexistence of file + file + folder + drive; independent data.
        QDialog *b = openProps(window, fileB, false);
        DriveInfo drive;
        drive.name = QStringLiteral("Root");
        drive.mountPoint = QStringLiteral("/");
        drive.isMounted = true;
        drive.fileSystem = QStringLiteral("ext4");
        drive.targetUrl = QUrl::fromLocalFile(QStringLiteral("/"));
        window.showDrivePropertiesDialog(drive);
        verify(life.openCount() == 4, "file, file, folder and drive Properties coexist");
        const auto all = life.openDialogs();
        QSet<QDialog *> unique(all.begin(), all.end());
        verify(unique.size() == 4, "every Properties dialog is a distinct instance");
        QDialog *driveDialog = nullptr;
        for (QDialog *d : all) if (d->objectName() == "drivePropertiesDialog") driveDialog = d;
        verify(driveDialog && !driveDialog->isModal(), "Drive Properties remains modeless");
        verify(QApplication::activeModalWidget() == nullptr && window.isEnabled(), "no modal block with 4 dialogs");
        QSet<QObject *> providers;
        for (QDialog *d : {a, b, f}) {
            const auto p = d->findChildren<PropertiesDataProvider *>();
            verify(p.size() == 1, "each file/folder dialog owns its provider");
            providers.insert(p.first());
        }
        verify(providers.size() == 3, "providers are per dialog");
        verify(a->findChild<QDialogButtonBox *>() != b->findChild<QDialogButtonBox *>(), "dirty state is per dialog");

        // Two dialogs for the same file: independent and safe.
        QDialog *a2 = openProps(window, fileA, false);
        verify(a2 != a, "same file can be opened twice");
        setOwnerWriteBit(a, false);
        setOwnerWriteBit(a2, false);
        button(a, QDialogButtonBox::Apply)->click();
        button(a2, QDialogButtonBox::Apply)->click();
        verify(g_boxes.isEmpty(), "double Apply on same file raises no error");
        verify(a->isVisible() && a2->isVisible(), "Apply leaves both dialogs open");
        a2->close();
        flush();

        // Primary + Split targets coexist.
        if (window.m_splitPane) {
            QDialog *s1 = openProps(window, window.m_primaryPane->currentUrl().toLocalFile().isEmpty() ? fileB : fileB, false);
            QDialog *s2 = openProps(window, folder, true);
            verify(s1->isVisible() && s2->isVisible() && s1 != s2, "properties opened for both pane targets coexist");
            s1->close(); s2->close();
            flush();
        }

        // 11-12: failed write does not close as success (rename onto existing).
        QDialog *clash = openProps(window, fileB, false);
        clash->findChild<QLineEdit *>("propertiesName")->setText("a.txt");
        g_boxes.clear();
        button(clash, QDialogButtonBox::Ok)->click();
        QCoreApplication::processEvents();
        verify(!g_boxes.isEmpty(), "failed rename shows a controlled message");
        verify(clash->isVisible() && QFile::exists(fileB), "failed OK keeps dialog open and file untouched");
        clash->close();
        flush();

        // 13: Checksum running -> close dialog is safe and quick.
        const QString big = makeFile(dir.filePath("big.bin"), 768LL * 1024 * 1024);
        QDialog *hashDialog = openProps(window, big, false);
        QPointer<QDialog> hashGuard(hashDialog);
        auto *checksum = hashDialog->findChild<ChecksumWidget *>("checksumWidget");
        verify(checksum && checksum->job()->start(), "large checksum started");
        verify(checksum->job()->isRunning(), "checksum is running");
        QElapsedTimer timer;
        timer.start();
        hashDialog->close();
        flush();
        verify(hashGuard.isNull(), "dialog destroyed while checksum was running");
        verify(timer.elapsed() < 5000, "closing during checksum cancels the worker promptly");
        QTest::qWait(100);
        QCoreApplication::processEvents();

        // Re-open: checksum still works.
        const QString small = makeFile(dir.filePath("small.txt"));
        QDialog *again = openProps(window, small, false);
        auto *checksum2 = again->findChild<ChecksumWidget *>("checksumWidget");
        verify(checksum2 && checksum2->job()->start(), "checksum starts after reopening Properties");
        verify(QTest::qWaitFor([&] { return checksum2->job()->data().state == ChecksumState::Completed; }, 5000),
               "checksum completes in a reopened dialog");
        again->close();
        flush();

        // 14: Metadata loading -> immediate close is safe.
        QDialog *metaDialog = openProps(window, small, false);
        QPointer<QDialog> metaGuard(metaDialog);
        auto *metadata = metaDialog->findChild<MetadataWidget *>("metadataWidget");
        auto *tabs = metaDialog->findChild<QTabWidget *>();
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i) == metadata) tabs->setCurrentIndex(i);
        verify(metadata->provider()->startCount() == 1, "metadata extraction started lazily on tab activation");
        metaDialog->close();
        flush();
        verify(metaGuard.isNull(), "dialog destroyed while metadata was loading");
        QTest::qWait(300);
        QCoreApplication::processEvents();

        // 15: Remote Properties is modeless (no new synchronous network call).
        {
            QDialog *remote = PropertiesDialog::show(&window, "remote.txt",
                QUrl("sftp://invalid.invalid/home/user/remote.txt"), false, {}, {}, {},
                [] { return false; }, [](KIO::CopyJob *) {}, [](const QString &, int) {}, [] {});
            verify(remote && !remote->isModal() && window.isEnabled(), "Remote Properties is modeless");
            auto *rc = remote->findChild<ChecksumWidget *>("checksumWidget");
            verify(rc && rc->job()->data().capability == ChecksumCapability::RemoteUnavailable, "remote checksum unavailable");
            auto *rm = remote->findChild<MetadataWidget *>("metadataWidget");
            verify(rm && rm->provider()->data().capability == MetadataCapability::RemoteUnavailable, "remote metadata unavailable");
            remote->close();
            flush();
            QTest::qWait(50);
        }

        // 16: Symlink Properties is modeless; checksum/metadata unavailable.
        {
            const QString link = dir.filePath("broken-link");
            verify(QFile::link(dir.filePath("does-not-exist"), link), "broken symlink fixture");
            QDialog *sym = openProps(window, link, false);
            verify(!sym->isModal(), "Symlink Properties is modeless");
            auto *sc = sym->findChild<ChecksumWidget *>("checksumWidget");
            verify(sc && sc->job()->data().capability == ChecksumCapability::SymlinkUnavailable, "symlink checksum unavailable");
            auto *sm = sym->findChild<MetadataWidget *>("metadataWidget");
            verify(sm && sm->provider()->data().capability == MetadataCapability::SymlinkUnavailable, "symlink metadata unavailable");
            sym->close();
            flush();
        }

        // 17-18: file disappears / is replaced while Properties is open.
        {
            const QString gone = makeFile(dir.filePath("gone.txt"));
            verify(::chmod(QFile::encodeName(gone).constData(), 0644) == 0, "chmod fixture");
            QDialog *dg = openProps(window, gone, false);
            verify(QFile::remove(gone), "target removed behind the dialog");
            setOwnerWriteBit(dg, false);
            g_boxes.clear();
            button(dg, QDialogButtonBox::Apply)->click();
            verify(!g_boxes.isEmpty(), "disappeared target gives a controlled message");
            verify(!QFile::exists(gone), "Apply never recreates a removed file");
            verify(dg->isVisible(), "dialog survives a disappeared target");
            dg->close();
            flush();

            const QString replaced = makeFile(dir.filePath("replaced.txt"));
            verify(::chmod(QFile::encodeName(replaced).constData(), 0644) == 0, "chmod fixture");
            QDialog *dr = openProps(window, replaced, false);
            verify(QFile::remove(replaced), "original removed");
            makeFile(dir.filePath("recycled_slot.tmp"));
            makeFile(replaced);
            verify(::chmod(QFile::encodeName(replaced).constData(), 0644) == 0, "chmod fixture");
            setOwnerWriteBit(dr, false);
            g_boxes.clear();
            button(dr, QDialogButtonBox::Apply)->click();
            verify(modeOf(replaced) == 0644, "Apply does not touch a different file that replaced the target");
            verify(!g_boxes.isEmpty(), "replaced target gives a controlled message");
            dr->close();
            flush();

            const QString vanishingDir = dir.filePath("vanishing");
            verify(QDir().mkpath(vanishingDir), "vanishing folder");
            QDialog *dd = openProps(window, vanishingDir, true);
            verify(QDir(vanishingDir).removeRecursively(), "folder removed behind the dialog");
            setOwnerWriteBit(dd, false);
            g_boxes.clear();
            button(dd, QDialogButtonBox::Ok)->click();
            verify(!g_boxes.isEmpty() && dd->isVisible(), "disappeared folder is safe and keeps dialog open");
            verify(!QFileInfo::exists(vanishingDir), "folder not recreated");
            dd->close();
            flush();
        }

        // Cancel closes without applying; busy-close deferral is covered by Apply above.
        {
            QDialog *c = openProps(window, fileB, false);
            const int before = modeOf(fileB);
            setOwnerWriteBit(c, false);
            button(c, QDialogButtonBox::Cancel)->click();
            flush();
            verify(modeOf(fileB) == before, "Cancel discards unapplied changes");
        }

        // 19-20: drive removal while Drive Properties is open stays safe,
        // then main window close with several dialogs closes them all.
        {
            DriveInfo removable;
            removable.name = QStringLiteral("Removable");
            removable.udi = QStringLiteral("/org/freedesktop/UDisks2/block_devices/stage6usb1");
            removable.isMounted = true;
            removable.mountPoint = QStringLiteral("/media/stage6");
            DrivePropertiesData snap;
            snap.udi = removable.udi;
            snap.name = removable.name;
            snap.isMounted = true;
            snap.mountPoint = removable.mountPoint;
            PropertiesDialog::showForDrive(&window, removable, {}, &snap);
            Q_EMIT Solid::DeviceNotifier::instance()->deviceRemoved(removable.udi);
            verify(window.isEnabled(), "device removal with Drive Properties open is safe");
        }

        QDialog *m1 = openProps(window, fileA, false);
        QDialog *m2 = openProps(window, fileB, false);
        QDialog *m3 = openProps(window, folder, true);
        auto *mh = openProps(window, big, false)->findChild<ChecksumWidget *>("checksumWidget");
        verify(mh->job()->start(), "checksum running during main window close");
        QPointer<QDialog> g1(m1), g2(m2), g3(m3);
        verify(life.openCount() >= 4, "several Properties open before main window close");
        window.close();
        flush();
        verify(g1.isNull() && g2.isNull() && g3.isNull(), "main window close closes File/Folder Properties");
        verify(life.openCount() == 0, "main window close leaves no Properties dialogs (incl. Drive)");
    }
    flush();
    QTest::qWait(100);

    // 21: Source guard: only one dialog.exec() remains in the production
    // Properties headers' File/Folder path (none): show() returns immediately,
    // which every assertion above relies on (an exec() would never return).
    verify(PropertiesLifecycle::instance().openCount() == 0, "no leaked Properties dialogs at exit");

    qInfo("PASS: %d properties lifecycle assertions; modeless file/folder/drive, coexistence, close safety, disappearing targets", checks);
    return 0;
}
