#include "propertiesxattrwidget.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QThread>
#include <QTest>
#include <atomic>
static int checks=0;
static void verify(bool ok,const char *why) { if(!ok) qFatal("FAIL: %s",why); ++checks; }
using S=PropertiesXattrWriteStatus;
using O=PropertiesXattrOperation;
struct FakeWriter {
    QMap<QByteArray,QByteArray> values;
    int calls=0,writes=0,closed=0,identities=0,flags=0,error=0,openError=0,inspectError=0;
    int replacedAt=0; bool missing=false,fdReplaced=false;
    PropertiesTargetCapabilities target() {
        PropertiesTargetCapabilities t; t.requestedUrl=t.normalizedUrl=QUrl::fromLocalFile("/fake"); t.isLocal=true;
        t.entryIdentity={true,1,1,S_IFREG}; t.userXattrEditable=PropertiesCapabilityState::Supported; return t;
    }
    PropertiesXattrWriter::Backend backend() {
        PropertiesXattrWriter::Backend b;
        b.identity.lstatFn=[this](const char *,struct stat *s) { ++calls; ++identities; if(missing) { errno=ENOENT; return -1; } s->st_dev=1; s->st_ino=replacedAt && identities>=replacedAt ? 2 : 1; s->st_mode=S_IFREG; return 0; };
        b.open=[this](const char *,int f) { ++calls; verify((f&O_NOFOLLOW) && (f&O_CLOEXEC) && (f&O_NONBLOCK) && !(f&O_TRUNC),"no-follow bounded non-destructive open flags"); flags=f; if(openError) { errno=openError; return -1; } return 42; };
        b.inspect=[this](int fd,struct stat *s) { ++calls; verify(fd==42,"inspect pinned descriptor"); if(inspectError) { errno=inspectError; return -1; } s->st_dev=1;s->st_ino=fdReplaced ? 2 : 1;s->st_mode=S_IFREG;return 0; };
        b.close=[this](int fd) { ++closed; verify(fd==42,"close pinned descriptor"); return 0; };
        b.set=[this](int fd,const char *n,const void *v,size_t size,int f) { ++writes;verify(fd==42,"set uses verified descriptor"); verify(QByteArray(n).startsWith("user."),"only user namespace reaches syscall"); flags=f; if(error) { errno=error;return -1; } if((f==XATTR_CREATE && values.contains(n)) || (f==XATTR_REPLACE && !values.contains(n))) { errno=f==XATTR_CREATE ? EEXIST : ENODATA;return -1; } values[n]=QByteArray(static_cast<const char *>(v),size);return 0; };
        b.remove=[this](int fd,const char *n) { ++writes;verify(fd==42,"remove uses verified descriptor");verify(QByteArray(n).startsWith("user."),"remove user namespace only"); if(error) { errno=error;return -1; } if(!values.remove(n)) { errno=ENODATA;return -1; } return 0; };return b;
    }
    PropertiesXattrWriteResult write(O op,const QByteArray &name="user.foo",const QByteArray &value="text") { identities=0;return PropertiesXattrWriter::write(target(),op,name,value,backend()); }
};
int main(int argc,char **argv) {
    QApplication app(argc,argv); FakeWriter f;
    verify(f.write(O::Add).status==S::Success && f.values["user.foo"]=="text" && f.flags==XATTR_CREATE,"valid add pure CREATE");
    const auto unicode=QString::fromUtf8("Zażółć gęślą jaźń").toUtf8();
    verify(f.write(O::Add,"user.unicode",unicode).status==S::Success && f.values["user.unicode"]==unicode,"Unicode exact bytes");
    verify(f.write(O::Add,"user.empty",{}).status==S::Success && f.values["user.empty"].size()==0,"zero-byte add");
    QByteArray parsed="sentinel";
    verify(PropertiesXattrWriter::parseHex("00 FF 01 7f 80 41",&parsed) && parsed==QByteArray::fromHex("00ff017f8041"),"binary hex exact");
    verify(f.write(O::Add,"user.binary",parsed).status==S::Success && f.values["user.binary"]==parsed,"add binary including NUL");
    for(const QString &invalid:{QStringLiteral("0"),QStringLiteral("xx"),QStringLiteral("00\nff"),QStringLiteral("00\tff"),QStringLiteral("0x00"),QString::fromUtf8("ＦＦ")}) { parsed="sentinel"; verify(!PropertiesXattrWriter::parseHex(invalid,&parsed) && parsed=="sentinel","hex invalid never partial parse"); }
    verify(PropertiesXattrWriter::parseHex("",&parsed) && parsed.isEmpty(),"empty hex allowed");
    verify(PropertiesXattrWriter::parseHex("aAbB",&parsed) && parsed==QByteArray::fromHex("aabb"),"contiguous hex pairs allowed");
    verify(!PropertiesXattrWriter::parseHex(QString(131074,'0'),&parsed),"hex bounded");
    verify(PropertiesXattrWriter::encodeText(QString::fromUtf8("one\ntwo\tZażółć"),&parsed) && parsed==QString::fromUtf8("one\ntwo\tZażółć").toUtf8(),"text whitespace exact no terminator");
    verify(PropertiesXattrWriter::encodeText(QString(QChar(0)),&parsed) && parsed==QByteArray(1,'\0'),"explicit text NUL stored as data");
    verify(!PropertiesXattrWriter::encodeText(QString(QChar(0xd800)),&parsed),"unpaired surrogate blocked");
    verify(!PropertiesXattrWriter::encodeText(QString(65537,'x'),&parsed),"text value bounded");
    for(const QByteArray &name:{QByteArray("foo"),QByteArray("user."),QByteArray("USER.foo"),QByteArray("security.foo"),QByteArray("trusted.foo"),QByteArray("system.foo"),QByteArray("other.foo"),QByteArray("user.a\0b",8),QByteArray("user.")+QByteArray::fromHex("ff"),QByteArray("user.")+QByteArray(251,'x')}) {
        for(auto op:{O::Add,O::Edit,O::Remove}) { const int before=f.calls;verify(f.write(op,name).status==S::InvalidName && f.calls==before,"invalid/protected name blocked before backend"); }
    }
    verify(PropertiesXattrWriter::validName(QByteArray("user.")+QByteArray(250,'x')),"255-byte name valid");
    verify(PropertiesXattrWriter::validName(QString::fromUtf8("user.zażółć").toUtf8()),"lossless Unicode name allowed");
    verify(PropertiesXattrWriter::validName("user. foo "),"name never trimmed");
    int before=f.calls;verify(f.write(O::Add,"user.large",QByteArray(65537,'x')).status==S::TooLarge && before==f.calls,"large raw value blocked before syscall");
    verify(f.write(O::Add).status==S::ConflictExists && f.values["user.foo"]=="text","CREATE conflict never overwrites");
    verify(f.write(O::Edit,"user.foo","new").status==S::Success && f.values["user.foo"]=="new" && f.flags==XATTR_REPLACE,"edit pure REPLACE");
    verify(f.write(O::Edit,"user.missing").status==S::ConflictMissing && !f.values.contains("user.missing"),"edit missing never creates");
    verify(f.write(O::Remove).status==S::Success && !f.values.contains("user.foo"),"remove specific name");
    verify(f.write(O::Remove).status==S::ConflictMissing,"remove missing stable conflict");
    for(int e:{EACCES,EPERM,ENOTSUP,EROFS,ENOSPC,EDQUOT,ENOENT,ENODATA,EEXIST,ERANGE,E2BIG,EIO}) { f.error=e; verify(f.write(O::Add,"user.err").status==PropertiesXattrWriter::errorStatus(e),"mutation errno mapping"); } f.error=0;
    for(int e:{EACCES,EROFS,EIO,ENOENT,ELOOP}) { f.openError=e;verify(f.write(O::Add,"user.open").status==(e==ELOOP ? S::Replaced : PropertiesXattrWriter::errorStatus(e)),"open errors stable"); } f.openError=0;
    f.inspectError=EIO;before=f.writes;verify(f.write(O::Add).status==S::Error && f.writes==before,"inspect failure prevents write"); f.inspectError=0;
    for(auto op:{O::Add,O::Edit,O::Remove}) {
        f.missing=true;before=f.writes;verify(f.write(op).status==S::Missing && f.writes==before,"missing target all operations blocked");f.missing=false;
        f.replacedAt=1;verify(f.write(op).status==S::Replaced && f.writes==before,"replaced target all operations blocked");
        f.replacedAt=0;f.fdReplaced=true;verify(f.write(op).status==S::Replaced && f.writes==before,"race between preflight and FD inspect blocked");f.fdReplaced=false;
        f.replacedAt=2;verify(f.write(op).status==S::Replaced && f.writes==before,"race after verified open before write blocked");f.replacedAt=0;
    }
    auto t=f.target();
    for(auto cap:{PropertiesCapabilityState::ReadOnly,PropertiesCapabilityState::PermissionDenied,PropertiesCapabilityState::Unsupported,PropertiesCapabilityState::Unknown}) { t.userXattrEditable=cap;before=f.calls;const auto result=PropertiesXattrWriter::write(t,O::Add,"user.foo",{},f.backend());verify(result.status!=S::Success && f.calls==before,"capability blocks backend"); }t=f.target();
    t.entryIdentity.valid=false;before=f.calls;verify(PropertiesXattrWriter::write(t,O::Add,"user.foo",{},f.backend()).status==S::Unknown && f.calls==before,"invalid identity blocked");
    for(const QString &scheme:{"sftp","smb","fish","ftp","webdav","webdavs","http","https","admin"}) { t=f.target();t.requestedUrl=QUrl(scheme+"://host/file");for(auto op:{O::Add,O::Edit,O::Remove}) {before=f.calls;verify(PropertiesXattrWriter::write(t,op,"user.foo",{},f.backend()).status==S::Unsupported && f.calls==before,"remote even local-derived capability zero backend calls");} }
    for(auto type:{S_IFLNK,S_IFIFO,S_IFCHR,S_IFSOCK}) { t=f.target();t.entryIdentity.type=type;before=f.calls;verify(PropertiesXattrWriter::write(t,O::Add,"user.foo",{},f.backend()).status==S::Unsupported && f.calls==before,"symlink/special no-follow no backend write"); }
    QTemporaryDir tmp; const QString path=tmp.filePath("target");QFile file(path);verify(file.open(QIODevice::WriteOnly) && file.write("bytes")==5,"create temp real fixture");file.close();
    auto real=PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(path));struct stat original{},after{};::lstat(QFile::encodeName(path),&original);
    const QByteArray key="user.thispc.stage3";bool supported=false;
    auto result=PropertiesXattrWriter::write(real,O::Add,key,"one");
    if(result.status==S::Success) {
        supported=true;verify(true,"real FD CREATE smoke");char buf[10]{};verify(::lgetxattr(QFile::encodeName(path),key,buf,sizeof(buf))==3 && QByteArray(buf,3)=="one","real exact one bytes");
        verify(PropertiesXattrWriter::write(real,O::Edit,key,"two").status==S::Success,"real FD REPLACE");verify(::lgetxattr(QFile::encodeName(path),key,buf,sizeof(buf))==3 && QByteArray(buf,3)=="two","real exact two bytes");
        verify(PropertiesXattrWriter::write(real,O::Remove,key).status==S::Success,"real FD remove");errno=0;verify(::lgetxattr(QFile::encodeName(path),key,nullptr,0)==-1 && errno==ENODATA,"real absence ENODATA");
        ::lstat(QFile::encodeName(path),&after);verify(original.st_mode==after.st_mode && original.st_size==after.st_size && original.st_mtim.tv_sec==after.st_mtim.tv_sec && original.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"real content/mode/mtime invariant ctime may change");verify(file.open(QIODevice::ReadOnly) && file.readAll()=="bytes","real content unchanged");file.close();
        verify(PropertiesXattrWriter::write(real,O::Add,key,QByteArray::fromHex("00ff01")).status==S::Success,"real binary add");verify(::lgetxattr(QFile::encodeName(path),key,buf,sizeof(buf))==3 && QByteArray(buf,3)==QByteArray::fromHex("00ff01"),"real binary exact bytes");
        const auto link=tmp.filePath("link"),broken=tmp.filePath("broken");::symlink(QFile::encodeName(path),QFile::encodeName(link));::symlink("absent",QFile::encodeName(broken));
        for(const auto &p:{link,broken}) {auto symlink=PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(p));for(auto op:{O::Add,O::Edit,O::Remove}) verify(PropertiesXattrWriter::write(symlink,op,key,"bad").status==S::Unsupported,"real symlink/broken link never followed");}
        verify(::lgetxattr(QFile::encodeName(path),key,buf,sizeof(buf))==3 && QByteArray(buf,3)==QByteArray::fromHex("00ff01"),"symlink target untouched");
        const auto saved=tmp.filePath("original");verify(QFile::rename(path,saved),"preserve original inode for replacement test");verify(file.open(QIODevice::WriteOnly) && file.write("replacement")==11,"replacement fixture");file.close();
        for(auto op:{O::Add,O::Edit,O::Remove}) verify(PropertiesXattrWriter::write(real,op,key,"bad").status==S::Replaced,"real replace-at-same-path blocked");verify(::llistxattr(QFile::encodeName(path),nullptr,0)==0,"replacement no attributes written");
        const auto dir=PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(tmp.path()));verify(PropertiesXattrWriter::write(dir,O::Add,key,"dir").status==S::Success,"directory FD mutation");verify(PropertiesXattrWriter::write(dir,O::Remove,key).status==S::Success,"directory FD remove");
    } else { verify(result.status==S::Unsupported || result.status==S::PermissionDenied,"real smoke only skips unsupported/denied environment");qInfo("SKIP real xattrs: %d",result.errorNumber); }
    std::atomic<int> delivered{0},workers{0};QSemaphore started,release;
    auto delayed=[&](const auto &,auto,const auto &,const auto &) {++workers;started.release();release.acquire();return PropertiesXattrWriteResult{S::Success};};
    PropertiesXattrMutationController controller(nullptr,delayed);QObject::connect(&controller,&PropertiesXattrMutationController::completed,&app,[&](const auto &){++delivered;});
    for(const auto &name:{QByteArray("security.foo"),QByteArray("trusted.foo"),QByteArray("system.foo"),QByteArray("user.")}) verify(!controller.submit(f.target(),O::Add,name) && workers==0,"controller rejects protected/invalid before injected writer");
    verify(controller.submit(f.target(),O::Add,"user.a"),"async accepted");verify(started.tryAcquire(1,3000),"worker entered");verify(!controller.submit(f.target(),O::Add,"user.a") && controller.busy(),"double-click serialized");controller.invalidate();release.release();verify(QTest::qWaitFor([&]{return !controller.busy();}),"stale worker completes");verify(delivered==0 && workers==1,"stale completion ignored");
    verify(controller.submit(f.target(),O::Add,"user.a"),"next operation after completion");verify(started.tryAcquire(1,3000),"next worker entered");release.release();verify(QTest::qWaitFor([&]{return delivered==1;}),"current completion delivered");
    auto *closing=new PropertiesXattrMutationController(nullptr,delayed);verify(closing->submit(f.target(),O::Add,"user.a"),"close operation accepted");verify(started.tryAcquire(1,3000),"close worker entered");delete closing;release.release();verify(QTest::qWaitFor([&]{return workers==3;}),"close while writing no UAF");
    // Reader/writer seams exercise fresh reload, failure consistency and protected UI guards.
    QMap<QByteArray,QByteArray> values{{"security.fake","s"},{"trusted.fake","t"},{"system.fake","y"}};
    std::atomic<int> reads{0},uiWrites{0};bool fail=false;
    auto reader=[&](const QUrl &,const auto &) {++reads;PropertiesXattrSnapshot s;s.status=values.isEmpty()?PropertiesXattrStatus::Empty:PropertiesXattrStatus::Ready;for(auto it=values.cbegin();it!=values.cend();++it){PropertiesXattrEntry e;e.name=it.key();e.raw=it.value();e.byteSize=e.raw.size();PropertiesXattrReader::present(e);s.entries.append(e);}return s;};
    auto writer=[&](const auto &,auto op,const auto &name,const auto &value){++uiWrites;if(fail)return PropertiesXattrWriteResult{S::PermissionDenied};if(op==O::Remove)values.remove(name);else values[name]=value;return PropertiesXattrWriteResult{S::Success};};
    QLocale::setDefault(QLocale(QLocale::English));PropertiesXattrWidget widget(f.target().requestedUrl,nullptr,f.target().entryIdentity,f.target(),writer,reader);widget.activate();auto *add=widget.findChild<QPushButton*>("propertiesXattrAdd");auto *edit=widget.findChild<QPushButton*>("propertiesXattrEdit");auto *remove=widget.findChild<QPushButton*>("propertiesXattrRemove");auto *table=widget.findChild<QTableWidget*>();
    verify(QTest::qWaitFor([&]{return add->isEnabled();}),"read loaded writable UI");verify(add->text()=="Add" && edit->text()=="Edit" && remove->text()=="Remove","EN labels");
    for(int row=0;row<3;++row) {table->setCurrentCell(row,0);verify(!edit->isEnabled() && !remove->isEnabled(),"protected button states");verify(!widget.mutate(O::Edit,table->item(row,0)->text().toUtf8(),"bad"),"protected keyboard/direct controller guard");}verify(uiWrites==0,"protected never passed to UI writer");
    for(auto op:{O::Add,O::Edit,O::Remove}) {int beforeRead=reads;verify(widget.mutate(op,"user.ui","hello"),"UI mutation accepted");verify(QTest::qWaitFor([&]{return reads>beforeRead && add->isEnabled();}),"Add Edit Remove each triggers fresh read");verify(op==O::Remove ? !values.contains("user.ui") : values["user.ui"]=="hello","fresh viewer kernel-equivalent state");}
    verify(widget.mutate(O::Add,"user.keep","keep"),"add failure fixture");verify(QTest::qWaitFor([&]{return add->isEnabled();}),"add fixture reload");fail=true;int beforeRead=reads;verify(widget.mutate(O::Edit,"user.keep","bad"),"failed write accepted");verify(QTest::qWaitFor([&]{return reads>beforeRead && add->isEnabled();}),"failed write refreshes viewer");verify(values["user.keep"]=="keep" && widget.findChild<QLabel*>("propertiesXattrWriteError")->text().contains("Permission denied"),"failure consistent and one inline error");
    fail=false;
    auto editorAction=[&](QPushButton *button,const std::function<void(QDialog *)> &action) {
        QTimer::singleShot(0,&widget,[action] {auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());verify(dialog!=nullptr,"real editor modal opened");action(dialog);});button->click();
    };
    for(const QString &badName:{QStringLiteral("security.bad"),QStringLiteral("trusted.bad"),QStringLiteral("user.")}) {
        const int beforeWrite=uiWrites;
        editorAction(add,[&](QDialog *d) {d->findChild<QLineEdit*>("propertiesXattrEditorName")->setText(badName);d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();verify(d->isVisible() && !d->findChild<QLabel*>("propertiesXattrEditorError")->text().isEmpty(),"invalid namespace stays in dialog with error");d->reject();});verify(uiWrites==beforeWrite,"invalid UI name never invokes writer");
    }
    editorAction(add,[&](QDialog *d) {d->findChild<QLineEdit*>("propertiesXattrEditorName")->setText("user.dialog");auto *format=d->findChild<QComboBox*>();verify(format->currentIndex()==0,"new editor defaults text");format->setCurrentIndex(1);auto *value=d->findChild<QPlainTextEdit*>("propertiesXattrEditorValue");value->setPlainText("0");d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();verify(d->isVisible(),"invalid hex cannot save partial input");value->setPlainText("00 ff 01");d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});verify(QTest::qWaitFor([&]{return add->isEnabled();}) && values["user.dialog"]==QByteArray::fromHex("00ff01"),"real Add dialog binary exact bytes");
    auto selectName=[&](const QString &name) {for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->text()==name)table->setCurrentCell(row,0);};selectName("user.dialog");
    editorAction(edit,[&](QDialog *d) {auto *format=d->findChild<QComboBox*>();verify(format->currentIndex()==1 && !format->isEnabled(),"existing binary fixed hex never text conversion");verify(d->findChild<QLineEdit*>()->isReadOnly(),"Edit name fixed no rename");verify(d->findChild<QPlainTextEdit*>("propertiesXattrEditorValue")->toPlainText()=="00 ff 01","Edit loads full raw binary");d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});verify(QTest::qWaitFor([&]{return add->isEnabled();}) && values["user.dialog"]==QByteArray::fromHex("00ff01"),"binary no-change Edit exact bytes");
    selectName("user.dialog");const int beforeRemove=uiWrites;QTimer::singleShot(0,&widget,[&]{auto *message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());verify(message && message->text().contains("user.dialog"),"Remove confirmation exact name");message->button(QMessageBox::No)->click();});remove->click();verify(uiWrites==beforeRemove && values.contains("user.dialog"),"Remove cancellation no write");
    QTimer::singleShot(0,&widget,[&]{auto *message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget());verify(message!=nullptr,"Remove confirmation opened");message->button(QMessageBox::Yes)->click();});remove->click();verify(QTest::qWaitFor([&]{return add->isEnabled();}) && !values.contains("user.dialog"),"confirmed Remove immediately refreshes");
    verify(widget.mutate(O::Add,"user.crlf","one\r\ntwo\rthree"),"mixed line endings fixture");verify(QTest::qWaitFor([&]{return add->isEnabled();}),"mixed line ending reload");selectName("user.crlf");editorAction(edit,[&](QDialog *d){d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});verify(QTest::qWaitFor([&]{return add->isEnabled();}) && values["user.crlf"]=="one\r\ntwo\rthree","unchanged text preserves original CRLF exact bytes");
    QLocale::setDefault(QLocale(QLocale::Polish));PropertiesXattrWidget polish(f.target().requestedUrl,nullptr,f.target().entryIdentity,f.target(),writer,reader);verify(polish.findChild<QPushButton*>("propertiesXattrAdd")->text()=="Dodaj" && polish.findChild<QPushButton*>("propertiesXattrEdit")->text()=="Edytuj" && polish.findChild<QPushButton*>("propertiesXattrRemove")->text()=="Usuń","PL labels");
    for(auto status:{S::InvalidName,S::TooLarge,S::Unsupported,S::PermissionDenied,S::ReadOnly,S::NoSpace,S::ConflictExists,S::ConflictMissing,S::Missing,S::Replaced,S::Unknown,S::Error}) {auto pl=PropertiesXattrWidget::writeStatusText(status);QLocale::setDefault(QLocale(QLocale::English));auto en=PropertiesXattrWidget::writeStatusText(status);verify(!pl.isEmpty() && !en.isEmpty() && pl!=en,"localized stable errors");QLocale::setDefault(QLocale(QLocale::Polish));}
    // Invalidate a widget while a write is actually paused in its backend.
    std::atomic<int> freshReads{0},freshCompletions{0}; QSemaphore widgetStarted,widgetRelease,widgetDone;
    auto changingReader=[&](const QUrl &url,const auto &) { ++freshReads;PropertiesXattrSnapshot out;out.status=PropertiesXattrStatus::Ready;PropertiesXattrEntry e;e.name="user.target";e.raw=url.path().toUtf8();e.byteSize=e.raw.size();PropertiesXattrReader::present(e);out.entries.append(e);return out; };
    auto changingWriter=[&](const auto &,auto,const auto &,const auto &) {widgetStarted.release();widgetRelease.acquire();widgetDone.release();return PropertiesXattrWriteResult{S::Success};};
    auto *changing=new PropertiesXattrWidget(f.target().requestedUrl,nullptr,f.target().entryIdentity,f.target(),changingWriter,changingReader);changing->activate();
    auto *changingAdd=changing->findChild<QPushButton*>("propertiesXattrAdd");verify(QTest::qWaitFor([&]{return changingAdd->isEnabled();}),"generation widget first read");
    QObject::connect(changing->findChild<PropertiesXattrMutationController*>(),&PropertiesXattrMutationController::completed,&app,[&](const auto &){++freshCompletions;});
    verify(changing->mutate(O::Add,"user.old","old"),"generation widget write started");verify(widgetStarted.tryAcquire(1,3000),"generation widget worker paused");verify(!changingAdd->isEnabled(),"buttons disabled during write");
    changing->setUrl(QUrl::fromLocalFile("/new"));changing->activate();verify(QTest::qWaitFor([&]{const auto *item=changing->findChild<QTableWidget*>()->item(0,1);return freshReads==2 && item && item->text()=="/new";}),"new generation read rendered before inspecting rows");widgetRelease.release();verify(widgetDone.tryAcquire(1,3000),"old mutation syscall completed");
    verify(QTest::qWaitFor([&]{return !changing->findChild<PropertiesXattrMutationController*>()->busy();}),"old controller completed after target change");verify(freshCompletions==0 && freshReads==2,"stale completion triggers no new target reload");
    verify(changing->findChild<QTableWidget*>()->item(0,1)->text()=="/new","new target rows not overwritten");
    verify(changing->mutate(O::Add,"user.close","closing"),"widget close write accepted");verify(widgetStarted.tryAcquire(1,3000),"widget close worker paused");delete changing;widgetRelease.release();verify(widgetDone.tryAcquire(1,3000),"widget deleted while mutation safely completes");verify(freshCompletions==0,"closed widget completion undelivered");
    QLocale::setDefault(QLocale(QLocale::English));
    interceptFileJobs=false;ThisPcWindow window(QUrl::fromLocalFile(tmp.path()));window.setSplitViewEnabled(true);
    for(int route=0;route<3;++route) {
        const bool split=route==1;const QUrl directory=route==2 ? QUrl("thispcsearch:/fixture") : QUrl::fromLocalFile(tmp.path());auto *list=split ? window.m_splitPane->listView() : window.m_directoryList;
        if(split){window.m_splitPane->setCurrentUrl(directory,false);window.m_splitPane->setViewMode(0);}else{window.m_primaryPane->setCurrentUrl(directory);window.setDirectoryViewMode(0);}window.setActivePane(split ? ThisPcWindow::PaneId::Split : ThisPcWindow::PaneId::Primary);list->clear();
        FileInfo info{QStringLiteral("target"),QString(),QString(),QUrl::fromLocalFile(path),false,11,0};list->addFileItem(info,QIcon(),"File","11",QString(),QString());list->selectionModel()->setCurrentIndex(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);window.showSelectedProperties();
        auto dialogs=PropertiesLifecycle::instance().openDialogs();verify(!dialogs.isEmpty(),"Primary Split Search Properties opened");auto *dialog=dialogs.last();auto *xattr=dialog->findChild<PropertiesXattrWidget*>();verify(xattr!=nullptr,"shared editing widget all routes");xattr->activate();verify(QTest::qWaitFor([&]{return xattr->findChild<QPushButton*>("propertiesXattrAdd")->isEnabled() || !supported;}),"Primary Split Search writable parity");dialog->close();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }window.close();
    qInfo("PASS: %d properties_xattr_edit assertions",checks);return 0;
}
