// All mutations below are restricted to disposable QTemporaryDir fixtures.
static int checks = 0;
static void verify(bool ok, const char *why) { if (!ok) qFatal("FAIL: %s", why); ++checks; }
static int modeFor(const QString &path) {
    struct stat st{};
    verify(::lstat(QFile::encodeName(path).constData(), &st) == 0, "lstat fixture");
    return st.st_mode & 07777;
}
static QLineEdit *numeric(QDialog *d) {
    auto *field = d->findChild<QLineEdit *>("propertiesNumericMode");
    if (!field) qFatal("numeric field missing from existing dialog");
    return field;
}
static QCheckBox *box(QDialog *d, int index) {
    auto *grid = qobject_cast<QGridLayout *>(numeric(d)->parentWidget()->layout());
    return qobject_cast<QCheckBox *>(grid->itemAtPosition(1 + index / 3, 1 + index % 3)->widget());
}
static void press(QDialog *d, QDialogButtonBox::StandardButton button) {
    d->findChild<QDialogButtonBox *>()->button(button)->click();
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale(QLocale::English));
    for (int value : {0644, 0755, 0000, 0777}) {
        int parsed = -1;
        verify(PropertiesPosixMode::parse(PropertiesPosixMode::format(value), &parsed) && parsed == value, "canonical round trip");
        verify(PropertiesPosixMode::fromBoxes(PropertiesPosixMode::boxes(value)) == value, "rwx mapping round trip");
    }
    for (const auto &text : {"", "0", "07", "075", "755", "0788", "0799", "-1", "+755", "0o755", "0x755", " 0755", "0755 ", "07 5", "04755"}) {
        int parsed = 123;
        verify(!PropertiesPosixMode::parse(QString::fromLatin1(text), &parsed) && parsed == 123, "invalid input never produces mode");
    }
    for (int specials : {04000, 02000, 01000, 07000})
        verify(PropertiesPosixMode::merge(specials | 0644, 0750) == (specials | 0750), "special bit preservation model");
    verify(PropertiesPosixMode::boxes(0644) == std::array<bool, 9>{true,true,false,true,false,false,true,false,false}, "0644 exact checkbox mapping");
    QTemporaryDir temp;
    verify(temp.isValid(), "disposable directory");
    const QString path = temp.filePath("file.txt");
    QFile file(path); verify(file.open(QIODevice::WriteOnly), "create fixture"); file.write("unchanged content\n"); file.close();
    verify(::chmod(QFile::encodeName(path).constData(), 0644) == 0, "initialize fixture");
    auto capabilities = PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(path));
    verify(PropertiesPosixMode::editable(QUrl::fromLocalFile(path), capabilities), "supported local numeric capability");
    for (auto state : {PropertiesCapabilityState::ReadOnly, PropertiesCapabilityState::PermissionDenied,
                       PropertiesCapabilityState::Unsupported, PropertiesCapabilityState::Unknown}) {
        capabilities.posixModeEditable = state;
        verify(!PropertiesPosixMode::editable(QUrl::fromLocalFile(path), capabilities), "unavailable numeric capability disabled");
    }
    capabilities.posixModeEditable = PropertiesCapabilityState::Supported;
    capabilities.entryIdentity.valid = false;
    verify(!PropertiesPosixMode::editable(QUrl::fromLocalFile(path), capabilities), "unknown identity cannot edit");
    int localCalls = 0;
    PropertiesCapabilityResolver::LocalSyscalls calls;
    calls.lstatFn = [&](const char *, struct stat *) { ++localCalls; return -1; };
    calls.statFn = [&](const char *, struct stat *) { ++localCalls; return -1; };
    calls.accessFn = [&](const char *, int) { ++localCalls; return -1; };
    calls.listXattrFn = [&](const char *, char *, size_t) -> ssize_t { ++localCalls; return -1; };
    for (const char *scheme : {"sftp", "smb", "ftp", "admin", "thispc", "trash"}) {
        QUrl remote(QString::fromLatin1(scheme) + "://host/path");
        const auto caps = PropertiesCapabilityResolver::resolve(remote, calls);
        verify(!PropertiesPosixMode::editable(remote, caps) && localCalls == 0, "remote numeric policy with zero local syscalls");
    }
    int refreshes = 0;
    auto open = [&](const QString &item, bool dir = false) {
        return PropertiesDialog::show(nullptr, QFileInfo(item).fileName(), QUrl::fromLocalFile(item), dir,
            {}, {}, {}, [] { qFatal("unexpected elevation"); return false; }, {}, {}, [&] { ++refreshes; });
    };
    auto close = [&](QDialog *d) { press(d, QDialogButtonBox::Cancel); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); };
    QTimer dismiss;
    dismiss.setInterval(5);
    QObject::connect(&dismiss, &QTimer::timeout, [] {
        if (auto *m = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) m->accept();
    }); dismiss.start();
    auto *d = open(path);
    verify(numeric(d)->isEnabled() && numeric(d)->text() == "0644", "owned local file capability and initial display");
    d->findChild<QTabWidget *>()->setCurrentIndex(3);
    verify(modeFor(path) == 0644, "open and tab switch do not mutate");
    int changes = 0;
    QObject::connect(numeric(d), &QLineEdit::textChanged, d, [&](const QString &) { ++changes; });
    numeric(d)->setText("0750");
    verify(changes == 1, "numeric sync has no recursive text signal");
    for (int i = 0; i < 9; ++i) verify(box(d,i)->isChecked() == bool(0750 & (0400 >> i)), "numeric to checkbox immediate");
    verify(modeFor(path) == 0644, "pending numeric edit has no write");
    close(d);
    verify(modeFor(path) == 0644, "Cancel discards numeric pending state");
    d = open(path); verify(numeric(d)->text() == "0644", "reopen restores actual mode");
    box(d,2)->setChecked(true);
    verify(numeric(d)->text() == "0744" && modeFor(path) == 0644, "checkbox to numeric pending sync");
    press(d,QDialogButtonBox::Apply);
    verify(modeFor(path) == 0744 && numeric(d)->text() == "0744", "Apply writes final state and rereads actual mode");
    struct stat before{}, after{}; ::lstat(QFile::encodeName(path).constData(), &before);
    press(d,QDialogButtonBox::Apply); ::lstat(QFile::encodeName(path).constData(), &after);
    verify(before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec, "no-change Apply has no redundant chmod");
    for (const char *bad : {"0788", "-1", "0x755", "", "075"}) {
        numeric(d)->setText(bad); const int count = refreshes;
        press(d,QDialogButtonBox::Apply); press(d,QDialogButtonBox::Ok);
        verify(d->isVisible() && refreshes == count && modeFor(path) == 0744, "invalid Apply/OK zero mutation and no success");
        verify(!d->findChild<QLabel *>("propertiesModeValidation")->text().isEmpty(), "clear validation state");
    }
    numeric(d)->setText("0644"); press(d,QDialogButtonBox::Ok);
    verify(!d->isVisible() && modeFor(path) == 0644, "OK saves and closes");
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    verify(file.open(QIODevice::ReadOnly) && file.readAll() == "unchanged content\n", "file content preserved"); file.close();
    const QString directory = temp.filePath("directory"); verify(QDir().mkdir(directory), "directory fixture");
    for (const auto &fixture : {path, directory}) {
        verify(::chmod(QFile::encodeName(fixture).constData(), 04755) == 0, "set disposable special bits");
        d = open(fixture, fixture == directory); numeric(d)->setText("0750"); press(d,QDialogButtonBox::Apply);
        verify(modeFor(fixture) == 04750 && numeric(d)->text() == "0750", "production KIO special bits preservation"); close(d);
    }
    verify(::chmod(QFile::encodeName(path).constData(), 04644) == 0, "checkbox special fixture");
    d = open(path); box(d,2)->setChecked(true);
    verify(numeric(d)->text()=="0744", "checkbox canonical display excludes preserved special bits");
    press(d,QDialogButtonBox::Apply);
    verify(modeFor(path)==04744,"checkbox production write preserves setuid"); close(d);
    for (int initial : {02750, 01777, 07700}) {
        verify(::chmod(QFile::encodeName(directory).constData(), initial) == 0, "directory special fixture");
        d = open(directory,true); numeric(d)->setText("0755"); press(d,QDialogButtonBox::Apply);
        verify(modeFor(directory) == PropertiesPosixMode::merge(initial,0755), "setgid/sticky production preservation"); close(d);
    }
    // Numeric chmod must use normal POSIX ACL mask semantics and refresh the table.
    verify(::chmod(QFile::encodeName(path).constData(),0666) == 0,"ACL fixture mode");
    AclData acl = AclProvider::load(path,true,false,false,false);
    verify(acl.canWrite(),"host ACL fixture supported");
    AclEntryData named; named.tag=AclTag::NamedUser; named.qualifier=uint(geteuid()); named.permissions=AclPermissions::fromBits(6);
    AclEntryData mask; mask.tag=AclTag::Mask; mask.permissions=AclPermissions::fromBits(6);
    acl.accessEntries << named << mask;
    verify(AclController::write(acl).success,"extended ACL fixture created");
    d=open(path); numeric(d)->setText("0640"); press(d,QDialogButtonBox::Apply);
    const auto actual = AclProvider::load(path,true,false,false,false);
    AclEditorWidget *editor = nullptr;
    for (auto *widget : d->findChildren<QGroupBox *>())
        if (auto *candidate = dynamic_cast<AclEditorWidget *>(widget)) editor = candidate;
    verify(editor != nullptr,"ACL editor found");
    const auto shown = editor->data();
    verify(actual.hasExtendedAccess && shown.hasExtendedAccess && modeFor(path)==0640,"numeric chmod keeps extended ACL and real mode");
    for (const auto &entry : shown.accessEntries) {
        if (entry.tag==AclTag::Mask) verify(entry.permissions.bits()==4,"ACL UI refresh shows kernel mask");
        if (entry.tag==AclTag::NamedUser) verify(entry.permissions.bits()==6 && entry.effectivePermissions.bits()==4,"ACL named stored/effective refresh");
    }
    close(d);
    const QString link=temp.filePath("link"), broken=temp.filePath("broken");
    verify(::symlink(QFile::encodeName(path).constData(),QFile::encodeName(link).constData())==0,"link fixture");
    verify(::symlink("missing",QFile::encodeName(broken).constData())==0,"broken fixture");
    for (const auto &item : {link,broken}) {
        d=open(item); verify(!numeric(d)->isEnabled(),"link numeric disabled");
        for (int i=0;i<9;++i) verify(!box(d,i)->isEnabled(),"link checkbox disabled");
        numeric(d)->setText("0777"); press(d,QDialogButtonBox::Apply);
        verify(modeFor(path)==0640,"symlink target untouched"); close(d);
    }
    d=open(path); numeric(d)->setText("0755");
    verify(QFile::rename(path,temp.filePath("old")),"hold original inode for replace fixture");
    verify(file.open(QIODevice::WriteOnly),"replacement fixture"); file.write("replacement"); file.close();
    verify(::chmod(QFile::encodeName(path).constData(),0600)==0,"replacement mode");
    press(d,QDialogButtonBox::Apply);
    verify(modeFor(path)==0600 && !numeric(d)->isEnabled(),"replacement write blocked and mode untouched"); close(d);
    d=open(path); numeric(d)->setText("0755"); verify(QFile::rename(path,temp.filePath("removed")),"missing fixture");
    const int count=refreshes; press(d,QDialogButtonBox::Apply);
    verify(!numeric(d)->isEnabled() && refreshes==count,"missing target blocked without success"); close(d);
    QLocale::setDefault(QLocale(QLocale::Polish));
    d=open(temp.filePath("removed")); numeric(d)->setText("0788");
    verify(d->findChild<QLabel *>("propertiesModeValidation")->text().startsWith("Wpisz"),"Polish validation");
    verify(numeric(d)->toolTip().contains("zachowywane"),"Polish scope tooltip"); close(d);
    QLocale::setDefault(QLocale(QLocale::English));
    const QString routePath = temp.filePath("removed");
    interceptFileJobs = false;
    ThisPcWindow window(QUrl::fromLocalFile(temp.path())); window.setSplitViewEnabled(true);
    for (int route = 0; route < 3; ++route) {
        const bool split = route == 1;
        const QUrl location = route == 2 ? QUrl("thispcsearch:/fixture") : QUrl::fromLocalFile(temp.path());
        auto *list = split ? window.m_splitPane->listView() : window.m_directoryList;
        if (split) { window.m_splitPane->setCurrentUrl(location,false); window.m_splitPane->setViewMode(0); }
        else { window.m_primaryPane->setCurrentUrl(location); window.setDirectoryViewMode(0); }
        window.setActivePane(split ? ThisPcWindow::PaneId::Split : ThisPcWindow::PaneId::Primary);
        list->clear();
        FileInfo info{QStringLiteral("removed"),QString(),QString(),QUrl::fromLocalFile(routePath),false,11,0};
        list->addFileItem(info,QIcon(),"File","11",QString(),QString());
        list->selectionModel()->setCurrentIndex(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
        window.showSelectedProperties();
        const auto dialogs = PropertiesLifecycle::instance().openDialogs();
        verify(!dialogs.isEmpty(), "Primary Split Search shared Properties route");
        auto *dialog = dialogs.last();
        verify(numeric(dialog)->isEnabled() && numeric(dialog)->text()=="0600", "Primary Split Search numeric capability/display parity");
        numeric(dialog)->setText("0750");
        verify(box(dialog,2)->isChecked() && !box(dialog,8)->isChecked(), "Primary Split Search numeric sync parity");
        close(dialog);
        verify(modeFor(routePath)==0600,"Primary Split Search Cancel has no mutation");
    }
    window.close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    verify(PropertiesLifecycle::instance().openCount()==0,"all modeless windows destroyed");
    qInfo("PASS: %d numeric POSIX mode assertions; production KIO, identity, ACL, special bits, UI sync",checks);
}
