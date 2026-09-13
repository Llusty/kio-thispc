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
    auto show = [&](const QString &item, bool directory, const std::function<void(QDialog *)> &exercise) {
        QTimer driver;
        driver.setInterval(10);
        QObject::connect(&driver, &QTimer::timeout, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            verify(!qobject_cast<QMessageBox *>(dialog), "no unexpected error dialog before properties");
            driver.stop();
            verify(dialog->windowTitle().startsWith("Properties"), "properties dialog opened");
            auto *tabs = dialog->findChild<QTabWidget *>();
            verify(tabs && tabs->count() == 2, "General and Permissions tabs");
            exercise(dialog);
        });
        QTimer errorGuard;
        errorGuard.setInterval(20);
        QObject::connect(&errorGuard, &QTimer::timeout, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                qFatal("Unexpected properties error: %s", qPrintable(box->text()));
        });
        errorGuard.start();
        driver.start();
        PropertiesDialog::show(nullptr, QFileInfo(item).fileName(), QUrl::fromLocalFile(item), directory,
            {}, {}, {}, [] { qFatal("Unexpected administrator request"); return false; },
            [&](KIO::CopyJob *job) { verify(job != nullptr, "rename supplies KIO CopyJob for Undo"); ++recordings; },
            [&](const QString &message, int) { verify(message == "Property changes saved.", "save status callback"); ++statuses; },
            [&] { ++refreshes; });
    };
    show(path, false, [&](QDialog *dialog) {
        dialog->findChild<QLineEdit *>()->setText("canceled.txt");
        setMode(dialog, 0600);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
    });
    verify(QFile::exists(path) && !QFile::exists(files.filePath("canceled.txt")), "Cancel preserves name");
    verify(modeFor(path) == 0640 && refreshes == 0 && recordings == 0, "Cancel preserves permissions and callbacks");

    show(path, false, [&](QDialog *dialog) {
        setMode(dialog, 0600);
        dialog->findChild<QLineEdit *>()->setText("applied.txt");
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
    verify(refreshes == 3 && statuses == 3, "folder save callbacks");
    qInfo("PASS: %d PropertiesDialog assertions; real KIO rename/chmod/stat, Apply/OK/Cancel, recursion", checks);
}
