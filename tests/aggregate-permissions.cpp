#include "aggregatepermissionswidget.h"
#include <QTest>
#include <QPointer>
#include <sys/acl.h>
static int checks = 0;
static void verify(bool value, const char *message) { if (!value) qFatal("FAIL: %s", message); ++checks; }
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp; verify(tmp.isValid(), "isolated fixture");
    const auto path = [&](const QString &name) { return QFile::encodeName(tmp.filePath(name)); };
    const auto url = [&](const QString &name) { return QUrl::fromLocalFile(tmp.filePath(name)); };
    const auto make = [&](const QString &name, mode_t mode) { QFile f(tmp.filePath(name)); verify(f.open(QIODevice::WriteOnly), "fixture file"); f.close(); verify(!::chmod(path(name), mode), "fixture chmod"); };
    const auto mode = [&](const QString &name) { struct stat st {}; verify(!::lstat(path(name), &st), "independent lstat"); return st.st_mode & 07777; };
    auto token = std::make_shared<std::atomic_bool>(false);
    make("a", 0644); make("b", 0600);
    auto model = AggregatePermissionsModel::capture({url("a"), url("b")});
    verify(model.allEditable(), "native local selection editable");
    verify(model.currentBit(0400) == 1 && model.currentBit(0040) == -1 && model.currentBit(0002) == 0, "set mixed unset distinct");
    auto results = AggregatePermissionsWriter::apply(model, {}, token);
    verify(results.size() == 2 && results[0].success && !results[0].changed && mode("a") == 0644 && mode("b") == 0600, "neutral model no mutation");
    for (int original = 0; original <= 07777; ++original) {
        for (int i = 0; i < 9; ++i) {
            const mode_t bit = 0400 >> i;
            const AggregatePermissionEdit set{bit, bit}, clear{bit, 0};
            verify(set.valid() && set.merged(original) == (original | bit), "selective set preserves other rwx and special");
            verify(clear.valid() && clear.merged(original) == (original & ~bit), "selective clear preserves other rwx and special");
        }
    }
    verify(!AggregatePermissionEdit{01000,0}.valid() && !AggregatePermissionEdit{1,2}.valid(), "invalid masks rejected");
    results = AggregatePermissionsWriter::apply(model, {0010,0010}, token);
    verify(results[0].success && results[1].success && mode("a") == 0654 && mode("b") == 0610, "descriptor selective real write");
    make("special", 07744);
    results = AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("special")}), {0001,0001}, token);
    verify(results[0].success && mode("special") == 07745, "real special bits preserved");
    QDir(tmp.path()).mkdir("folder"); make("folder/child", 0600); ::chmod(path("folder"), 0700);
    results = AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("folder"),url("folder/child")}), {0040,0040}, token);
    verify(results[0].success && results[1].success && mode("folder") == 0740 && mode("folder/child") == 0640, "overlapping selected entries both direct");
    results = AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("folder")}), {0010,0010}, token);
    verify(results[0].success && mode("folder") == 0750 && mode("folder/child") == 0640, "directory traverse no recursion");
    verify(!::symlink("a",path("link")) && !::symlink("missing",path("broken")), "symlink fixtures");
    for (const auto &name : {QString("link"),QString("broken")}) {
        auto blocked = AggregatePermissionsModel::capture({url("a"),url(name)});
        results = AggregatePermissionsWriter::apply(blocked, {0002,0002}, token);
        verify(!blocked.allEditable() && !results[0].success && !results[1].success && mode("a") == 0654, "mixed symlink selection entirely disabled target unchanged");
    }
    verify(!QFileInfo::exists(tmp.filePath("missing")), "broken target never created");
    int called = 0;
    AggregatePermissionsWriter::Hooks hooks;
    hooks.beforeValidation = [&](int) { ++called; };
    hooks.chmod = [&](int, mode_t) { ++called; errno=EROFS; return -1; };
    auto remote = AggregatePermissionsModel::capture({QUrl("sftp://host/a"), QUrl("admin:///tmp/a"), QUrl("thispc:/drive"),QUrl("file://server/shared"),url("a")});
    results = AggregatePermissionsWriter::apply(remote,{1,1},token,hooks);
    verify(!remote.allEditable() && results.size() == 5 && called == 0 && mode("a") == 0654, "remote mixed backend no writer syscalls");
    auto readonly = AggregatePermissionsModel::capture({url("a")}); readonly.items[0].reason="Read-only mount";
    results=AggregatePermissionsWriter::apply(readonly,{1,1},token,hooks);
    verify(!results[0].success && called==0, "readonly policy blocks before mutation");
    auto invalid = AggregatePermissionsModel::capture({url("a")}); invalid.items[0].mountId++;
    results=AggregatePermissionsWriter::apply(invalid,{1,1},token);
    verify(!results[0].success && !results[0].changed, "mount ID loss rejected");
    invalid=AggregatePermissionsModel::capture({url("a")}); invalid.items[0].mountRecord += " altered";
    results=AggregatePermissionsWriter::apply(invalid,{1,1},token);
    verify(!results[0].success && !results[0].changed, "mount replacement/options changed rejected");
    invalid=AggregatePermissionsModel::capture({url("a")}); invalid.items[0].root.identity.st_ino++;
    results=AggregatePermissionsWriter::apply(invalid,{1,1},token);
    verify(!results[0].success && !results[0].changed, "inode mismatch rejected");
    make("gone",0600); invalid=AggregatePermissionsModel::capture({url("gone")}); QFile::rename(tmp.filePath("gone"),tmp.filePath("held"));
    results=AggregatePermissionsWriter::apply(invalid,{1,1},token); verify(!results[0].success && mode("held")==0600,"move rejected");
    make("gone",0644); results=AggregatePermissionsWriter::apply(invalid,{1,1},token); verify(!results[0].success && mode("gone")==0644,"replacement rejected");
    QFile::remove(tmp.filePath("gone")); results=AggregatePermissionsWriter::apply(invalid,{1,1},token); verify(!results[0].success,"disappearance rejected");
    QDir(tmp.path()).mkdir("parent"); make("parent/file",0600); invalid=AggregatePermissionsModel::capture({url("parent/file")});
    QDir(tmp.path()).rename("parent","old-parent"); QDir(tmp.path()).mkdir("parent"); verify(!::link(path("old-parent/file"),path("parent/file")),"same inode new parent fixture");
    results=AggregatePermissionsWriter::apply(invalid,{1,1},token); verify(!results[0].success && mode("parent/file")==0600,"parent replacement despite same inode rejected");
    model=AggregatePermissionsModel::capture({url("a"),url("b")});
    hooks={}; hooks.beforeValidation=[&](int i) { if(i==1) token->store(true); };
    results=AggregatePermissionsWriter::apply(model,{0001,0001},token,hooks);
    verify(results[0].success && !results[1].success && mode("a")==0655 && mode("b")==0610,"cancellation partial per item"); token->store(false);
    model=AggregatePermissionsModel::capture({url("a"),url("b")}); hooks={};
    hooks.beforeValidation=[&](int i) { if(i==1) QFile::rename(tmp.filePath("b"),tmp.filePath("b-held")); };
    results=AggregatePermissionsWriter::apply(model,{0002,0002},token,hooks);
    verify(results[0].success && results[0].changed && !results[1].success && !results[1].changed && mode("b-held")==0610,"partial success and lost second root");
    hooks={}; hooks.chmod=[](int,mode_t) {errno=EROFS;return -1;};
    results=AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("a")}),{0004,0},token,hooks);
    verify(!results[0].success && !results[0].changed,"kernel readonly error breakdown");
    make("acl",0640);
    acl_t acl=acl_from_text("user::rw-,user:12345:rwx,group::r--,mask::r--,other::---");
    verify(acl && !acl_set_file(path("acl"),ACL_TYPE_ACCESS,acl),"extended ACL fixture"); acl_free(acl);
    results=AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("acl")}),{0010,0010},token);
    verify(results[0].success && mode("acl")==0650,"group rwx changes ACL mask");
    acl=acl_get_file(path("acl"),ACL_TYPE_ACCESS); ssize_t textSize=0; char *text=acl_to_text(acl,&textSize);
    verify(text && QByteArray(text,textSize).contains("user:12345:rwx") && QByteArray(text,textSize).contains("mask::r-x"),"named ACL retained mask changed kernel semantics"); acl_free(text); acl_free(acl);

    acl=acl_from_text("user::rwx,group::r-x,other::---");
    verify(acl && !acl_set_file(path("folder"),ACL_TYPE_DEFAULT,acl),"directory default ACL fixture"); acl_free(acl);
    acl=acl_get_file(path("folder"),ACL_TYPE_DEFAULT); text=acl_to_text(acl,&textSize); QByteArray defaultBefore(text,textSize); acl_free(text); acl_free(acl);
    results=AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("folder")}),{0001,0001},token);
    acl=acl_get_file(path("folder"),ACL_TYPE_DEFAULT); text=acl_to_text(acl,&textSize);
    verify(results[0].success && QByteArray(text,textSize)==defaultBefore && mode("folder/child")==0640,"default ACL and children untouched"); acl_free(text); acl_free(acl);
    verify(!::link(path("a"),path("alias")),"hardlink selected alias fixture");
    results=AggregatePermissionsWriter::apply(AggregatePermissionsModel::capture({url("a"),url("alias"),url("a")}),{0040,0},token);
    verify(results.size()==3 && results[0].success && results[1].success && results[2].success && results[0].changed && !results[1].changed && !results[2].changed,"hardlink and duplicate selected entries one mutation");
    model=AggregatePermissionsModel::capture({url("a")}); token->store(true); called=0; hooks={}; hooks.chmod=[&](int,mode_t){++called;return 0;};
    results=AggregatePermissionsWriter::apply(model,{1,0},token,hooks);
    verify(!results[0].success && !results[0].changed && called==0,"pre-cancel no writes"); token->store(false);
    results=AggregatePermissionsWriter::apply(model,{01000,01000},token,hooks);
    verify(!results[0].success && called==0,"invalid mask never calls writer");
    hooks={}; hooks.beforeValidation=[&](int){::chmod(path("a"),0600);};
    results=AggregatePermissionsWriter::apply(model,{1,1},token,hooks);
    verify(!results[0].success && mode("a")==0600,"concurrent mode change ctime guard prevents stale write");
    model=AggregatePermissionsModel::capture({url("a")}); hooks={};
    hooks.beforeValidation=[&](int){QFile::rename(tmp.filePath("a"),tmp.filePath("a-held")); ::symlink("b-held",path("a"));};
    results=AggregatePermissionsWriter::apply(model,{1,1},token,hooks);
    verify(!results[0].success && mode("b-held")==0610,"symlink substitution before opening never touches target");
    QFile::remove(tmp.filePath("a"));
    // GUI: scan guard, declined confirmation, successful Apply/read-back, OK,
    // Cancel and lifetime. Confirmation remains real and default-No.
    make("ui-a",0644); make("ui-b",0600);
    auto *dialog=new AggregatePropertiesDialog({url("ui-a"),url("ui-b")}); dialog->show();
    auto *choice=dialog->findChild<QComboBox*>("bulkChange8");
    auto *buttons=dialog->findChild<QDialogButtonBox*>();
    verify(choice && !choice->isEnabled() && !buttons->button(QDialogButtonBox::Apply)->isEnabled(),"editing disabled during scan");
    for(int i=0;i<500 && !choice->isEnabled();++i) QTest::qWait(10);
    verify(choice->isEnabled() && choice->currentIndex()==0,"neutral after scan");
    verify(dialog->findChild<QLabel*>("bulkCurrent3")->text().contains(trLocal("mieszane","mixed")),"GUI current mixed separate from neutral edit");
    choice->setCurrentIndex(1); verify(buttons->button(QDialogButtonBox::Apply)->isEnabled(),"explicit bit edit enables Apply");
    const auto answer = [&](QMessageBox::StandardButton response) {
        QTimer::singleShot(0, [&,response] {
            auto *box=dialog->findChild<QMessageBox*>("bulkPermissionConfirmation");
            verify(box && box->defaultButton()==box->button(QMessageBox::No),"explicit confirmation defaults No");
            box->button(response)->click();
        });
    };
    answer(QMessageBox::No); buttons->button(QDialogButtonBox::Ok)->click();
    verify(dialog->isVisible() && mode("ui-a")==0644 && mode("ui-b")==0600,"declined OK no writes no close");
    answer(QMessageBox::Yes); buttons->button(QDialogButtonBox::Apply)->click();
    verify(!choice->isEnabled(),"edits disabled while applying");
    for(int i=0;i<500 && !choice->isEnabled();++i) QTest::qWait(10);
    verify(mode("ui-a")==0645 && mode("ui-b")==0601 && dialog->isVisible(),"Apply async updates selective modes keeps dialog");
    verify(choice->currentIndex()==0 && !buttons->button(QDialogButtonBox::Apply)->isEnabled(),"Apply resets to neutral");
    verify(dialog->findChild<QLabel*>("aggregateSummary")->text()==trLocal("Zakończono","Complete"),"own chmod does not invalidate General scan");
    choice->setCurrentIndex(2); answer(QMessageBox::Yes); buttons->button(QDialogButtonBox::Apply)->click();
    for(int i=0;i<500 && !choice->isEnabled();++i) QTest::qWait(10);
    verify(mode("ui-a")==0644 && mode("ui-b")==0600,"second Apply refreshed snapshot");
    choice->setCurrentIndex(1); QPointer<QDialog> life(dialog); buttons->button(QDialogButtonBox::Cancel)->click();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    verify(!life && mode("ui-a")==0644 && mode("ui-b")==0600,"Cancel pending edit no write modeless delete");
    dialog=new AggregatePropertiesDialog({url("ui-a"),url("ui-b")}); dialog->show();
    buttons=dialog->findChild<QDialogButtonBox*>(); buttons->button(QDialogButtonBox::Cancel)->click();
    QTest::qWait(100); verify(!dialog->findChild<QComboBox*>("bulkChange0")->isEnabled(),"scan Cancel permanently disables edit");
    dialog->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    dialog=new AggregatePropertiesDialog({url("ui-a"),url("ui-b")}); dialog->show();
    choice=dialog->findChild<QComboBox*>("bulkChange8"); buttons=dialog->findChild<QDialogButtonBox*>();
    for(int i=0;i<500 && !choice->isEnabled();++i) QTest::qWait(10);
    choice->setCurrentIndex(1); life=dialog; answer(QMessageBox::Yes); buttons->button(QDialogButtonBox::Ok)->click();
    for(int i=0;i<500 && life;++i) QTest::qWait(10);
    verify(!life && mode("ui-a")==0645 && mode("ui-b")==0601,"confirmed OK writes verified then closes");
    dialog=new AggregatePropertiesDialog({url("ui-a"),url("ui-b")}); dialog->show();
    choice=dialog->findChild<QComboBox*>("bulkChange8");
    for(int i=0;i<500 && !choice->isEnabled();++i) QTest::qWait(10);
    QFile content(tmp.filePath("ui-a")); verify(content.open(QIODevice::Append) && content.write("changed content")>0,"external content-change fixture"); content.close();
    Q_EMIT dialog->findChild<SolidDeviceMonitor*>()->devicesChanged();
    verify(!choice->isEnabled() && dialog->findChild<QLabel*>("aggregateSummary")->text().contains(trLocal("Wynik częściowy","Partial results")),"external content change invalidates General and edit");
    dialog->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    dialog=new AggregatePropertiesDialog({url("ui-a"),url("ui-b")}); life=dialog; dialog->show(); dialog->close();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); QTest::qWait(30);
    verify(!life,"close during scan safe lifetime");
    qInfo("PASS: %d aggregate permissions checks",checks);
}
