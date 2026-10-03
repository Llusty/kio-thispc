/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Real KIO operations are restricted to this suite's QTemporaryDir.
static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static int modeFor(const QString &path)
{
    struct stat info {};
    verify(::stat(QFile::encodeName(path).constData(), &info) == 0, "stat disposable item");
    return info.st_mode & 0777;
}

static void setMode(QDialog *dialog, int mode)
{
    auto *group = dialog->findChild<QGroupBox *>();
    verify(group != nullptr, "permission group exists");
    auto *grid = qobject_cast<QGridLayout *>(group->layout());
    verify(grid != nullptr, "permission grid exists");
    int bit = 0400;
    for (int row = 1; row <= 3; ++row) {
        for (int column = 1; column <= 3; ++column, bit >>= 1) {
            auto *box = qobject_cast<QCheckBox *>(grid->itemAtPosition(row, column)->widget());
            verify(box && box->isEnabled(), "owned file permissions editable");
            box->setChecked(mode & bit);
        }
    }
}

static QTableWidget *accessAclTable(QDialog *dialog)
{
    for (auto *table : dialog->findChildren<QTableWidget *>())
        if (table->columnCount() == 3 && table->horizontalHeaderItem(1)
            && (table->horizontalHeaderItem(1)->text() == "Stored" || table->horizontalHeaderItem(1)->text() == "Zapisane"))
            return table;
    return nullptr;
}

static void setAclStored(QDialog *dialog, AclTag tag, const QString &permissions)
{
    auto *table = accessAclTable(dialog);
    verify(table != nullptr, "access ACL table exists");
    for (int row = 0; row < table->rowCount(); ++row) {
        if (AclTag(table->item(row, 0)->data(Qt::UserRole).toInt()) == tag) {
            table->item(row, 1)->setText(permissions);
            return;
        }
    }
    qFatal("FAIL: requested ACL row is missing");
}

static int aclBits(const AclData &data, AclTag tag, bool effective = false)
{
    for (const auto &entry : data.accessEntries)
        if (entry.tag == tag) return effective ? entry.effectivePermissions.bits() : entry.permissions.bits();
    return -1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-properties-test");
    QCoreApplication::setApplicationName("properties-test");
    QTemporaryDir files;
    verify(files.isValid(), "temporary directory");
    QString path = files.filePath("original.txt");
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly), "disposable file created");
    file.write("properties regression\n");
    file.close();
    verify(::chmod(QFile::encodeName(path).constData(), 0640) == 0, "initial mode");
    int refreshes = 0, recordings = 0, statuses = 0;
    // Properties is modeless (0.37 Stage 6): show() returns immediately with
    // a live dialog, the main application stays interactive, and no modal
    // widget may exist. The helper drives the dialog synchronously and then
    // flushes deferred deletion so every dialog's destruction is checked.
    auto show = [&](const QString &item, bool directory, const std::function<void(QDialog *)> &exercise) {
        QTimer errorGuard;
        errorGuard.setInterval(20);
        QObject::connect(&errorGuard, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                qFatal("Unexpected properties error: %s", qPrintable(box->text()));
        });
        errorGuard.start();
        QDialog *dialog = PropertiesDialog::show(nullptr, QFileInfo(item).fileName(), QUrl::fromLocalFile(item), directory,
            {}, {}, {}, [] { qFatal("Unexpected administrator request"); return false; },
            [&](KIO::CopyJob *job) { verify(job != nullptr, "rename supplies KIO CopyJob for Undo"); ++recordings; },
            [&](const QString &message, int) { verify(message == "Property changes saved.", "save status callback"); ++statuses; },
            [&] { ++refreshes; });
        QPointer<QDialog> guard(dialog);
        verify(dialog != nullptr && dialog->isVisible(), "properties dialog opened and returned immediately");
        verify(!dialog->isModal() && QApplication::activeModalWidget() == nullptr,
               "file/folder Properties is modeless and creates no modal widget");
        QCoreApplication::processEvents();
        verify(!qobject_cast<QMessageBox *>(dialog), "no unexpected error dialog before properties");
        verify(dialog->windowTitle().startsWith("Properties"), "properties dialog opened");
        auto *tabs = dialog->findChild<QTabWidget *>();
        verify(tabs && tabs->count() == 4, "General, Checksums, Metadata and Permissions tabs");
        auto *checksum = dialog->findChild<ChecksumWidget *>("checksumWidget");
        verify(checksum && checksum->job()->data().state == ChecksumState::Idle,
               "opening Properties does not start checksum calculation");
        auto *metadata = dialog->findChild<MetadataWidget *>("metadataWidget");
        verify(metadata && metadata->provider()->data().state == MetadataState::Idle
               && metadata->provider()->startCount() == 0,
               "opening Properties does not start metadata extraction");
        exercise(dialog);
        if (guard) dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "closed Properties dialog is destroyed (WA_DeleteOnClose)");
    };
    show(path, false, [&](QDialog *dialog) {
        bool hasSizeOnDisk = false;
        bool hasInode = false;
        bool hasOwner = false;
        bool hasGroup = false;
        bool hasMetadataChanged = false;
        for (auto *label : dialog->findChildren<QLabel *>()) {
            const QString text = label->text();
            if (text.contains("Size on disk:") || text.contains("Rozmiar na dysku:")) hasSizeOnDisk = true;
            if (text.contains("Inode:")) hasInode = true;
            if (text.contains("Owner:") || text.contains("Właściciel:")) hasOwner = true;
            if (text.contains("Group:") || text.contains("Grupa:")) hasGroup = true;
            if (text.contains("Metadata changed:") || text.contains("Zmiana metadanych:")) hasMetadataChanged = true;
        }
        verify(hasSizeOnDisk, "dialog displays Size on disk label");
        verify(hasInode, "dialog displays Inode label");
        verify(hasOwner, "dialog displays Owner label");
        verify(hasGroup, "dialog displays Group label");
        verify(hasMetadataChanged, "dialog displays Metadata changed label");

        auto *checksum = dialog->findChild<ChecksumWidget *>("checksumWidget");
        verify(checksum && checksum->job()->start(), "checksum can start from Properties");

        dialog->findChild<QLineEdit *>("propertiesName")->setText("canceled.txt");
        setMode(dialog, 0600);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
    });
    verify(QFile::exists(path) && !QFile::exists(files.filePath("canceled.txt")), "Cancel preserves name");
    verify(modeFor(path) == 0640 && refreshes == 0 && recordings == 0, "Cancel preserves permissions and callbacks");

    show(path, false, [&](QDialog *dialog) {
        setMode(dialog, 0600);
        dialog->findChild<QLineEdit *>("propertiesName")->setText("applied.txt");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        buttons->button(QDialogButtonBox::Apply)->click();
        verify(dialog->isVisible(), "Apply leaves dialog open");
        verify(!QFile::exists(path), "Apply renames original");
        path = files.filePath("applied.txt");
        verify(modeFor(path) == 0600, "Apply writes POSIX permissions");
        verify(recordings == 1 && refreshes == 1 && statuses == 1, "Apply callbacks");
        bool verified = false;
        for (auto *label : dialog->findChildren<QLabel *>())
            verified |= label->text().contains("saved and verified");
        verify(verified, "dialog reports successful permission read-back");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    verify(QFile::exists(path) && modeFor(path) == 0600, "Cancel after Apply keeps saved changes");
    show(path, false, [&](QDialog *dialog) {
        setMode(dialog, 0644);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        verify(!dialog->isVisible(), "OK closes dialog");
    });
    verify(modeFor(path) == 0644 && recordings == 1 && refreshes == 2, "OK saves without another rename");

    const auto makeExtendedAclFixture = [&](const QString &fileName) {
        const QString aclPath = files.filePath(fileName);
        QFile aclFile(aclPath);
        verify(aclFile.open(QIODevice::WriteOnly), "ACL dialog fixture created");
        aclFile.write("acl dialog regression\n"); aclFile.close();
        verify(::chmod(QFile::encodeName(aclPath).constData(), 0666) == 0, "ACL dialog fixture initial mode");
        AclData data = AclProvider::load(aclPath, true, false, false, false);
        verify(data.canWrite() && !data.hasExtendedAccess, "ACL dialog fixture starts minimal");
        AclEntryData named; named.tag = AclTag::NamedUser; named.qualifier = uint(geteuid()); named.permissions = AclPermissions::fromBits(6);
        AclEntryData mask; mask.tag = AclTag::Mask; mask.permissions = AclPermissions::fromBits(6);
        data.accessEntries << named << mask;
        verify(AclController::write(data).success, "ACL dialog fixture extended through controller");
        return aclPath;
    };

    const QString aclApplyPath = makeExtendedAclFixture("acl-apply.txt");
    show(aclApplyPath, false, [&](QDialog *dialog) {
        setAclStored(dialog, AclTag::NamedUser, "rwx");
        setAclStored(dialog, AclTag::Mask, "r-x");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        verify(dialog->isVisible(), "ACL Apply leaves dialog open");
        const AclData saved = AclProvider::load(aclApplyPath, true, false, false, false);
        verify(aclBits(saved, AclTag::Mask) == 5, "ACL Apply preserves explicit mask without chmod overwrite");
        verify(aclBits(saved, AclTag::NamedUser) == 7 && aclBits(saved, AclTag::NamedUser, true) == 5,
               "ACL Apply preserves named user stored/effective permissions");
        verify(aclBits(saved, AclTag::GroupObject) == 6, "ACL Apply preserves group object");
        verify(((modeFor(aclApplyPath) >> 3) & 7) == 5, "ACL Apply mode group bits reflect mask");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
    });

    const QString aclOkPath = makeExtendedAclFixture("acl-ok.txt");
    show(aclOkPath, false, [&](QDialog *dialog) {
        setAclStored(dialog, AclTag::NamedUser, "rwx");
        setAclStored(dialog, AclTag::Mask, "r-x");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        verify(!dialog->isVisible(), "ACL OK closes dialog");
    });
    const AclData okSaved = AclProvider::load(aclOkPath, true, false, false, false);
    verify(aclBits(okSaved, AclTag::Mask) == 5 && aclBits(okSaved, AclTag::NamedUser, true) == 5,
           "ACL OK preserves explicit mask and effective permissions");

    const QString folder = files.filePath("folder");
    verify(QDir().mkpath(folder + "/nested"), "disposable nested folders created");
    show(folder, true, [&](QDialog *dialog) {
        setMode(dialog, 0700);
        QCheckBox *recursive = nullptr;
        for (auto *box : dialog->findChildren<QCheckBox *>())
            if (box->text().startsWith("Apply changes to all")) recursive = box;
        verify(recursive && recursive->isEnabled(), "folder supports recursive changes");
        recursive->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    verify(modeFor(folder) == 0700 && modeFor(folder + "/nested") == 0700, "recursive chmod on disposable folders");
    verify(refreshes == 5 && statuses == 5, "folder save callbacks");
    qInfo("PASS: %d PropertiesDialog assertions; real KIO rename/chmod/stat, Apply/OK/Cancel, recursion", checks);
}
