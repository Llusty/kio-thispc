/* 0.40 Stage 6: shared production routes, mutation interactions and loss/lifetime. */
#include <QTest>
#include <sys/xattr.h>
static int checks=0;
static void verify(bool ok,const char *message) { ++checks; if(!ok) qFatal("FAIL: %s",message); }
static void drain() { QCoreApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); }
static QString make(const QString &path) { QFile f(path); verify(f.open(QIODevice::WriteOnly),"fixture created"); verify(f.write("stage6\n")==7,"fixture content"); f.close(); verify(::chmod(QFile::encodeName(path),0644)==0,"fixture mode"); return path; }
static int mode(const QString &path) { struct stat s{}; verify(::lstat(QFile::encodeName(path),&s)==0,"lstat fixture"); return s.st_mode&07777; }
static void press(QDialog *d,QDialogButtonBox::StandardButton b) { d->findChild<QDialogButtonBox*>()->button(b)->click(); }
static QDialog *open(ThisPcWindow &w,const QString &path) { w.showPropertiesDialog(QFileInfo(path).fileName(),QUrl::fromLocalFile(path),false,{},{},{}); auto ds=PropertiesLifecycle::instance().openDialogs(); verify(!ds.isEmpty(),"shared dialog registered"); return ds.last(); }
static void ready(QDialog *d) { auto *h=d->findChild<PropertiesHiddenWidget*>(); verify(h && QTest::qWaitFor([&]{return h->loaded();},5000),"KIO hidden read completes"); auto *x=d->findChild<PropertiesXattrWidget*>(); x->activate(); verify(QTest::qWaitFor([&]{return x->findChild<QPushButton*>("propertiesXattrAdd")->isEnabled();},5000),"real local xattrs ready/editable"); }
static AclEditorWidget *aclWidget(QDialog *d) { for(auto *w:d->findChildren<QWidget*>()) if(auto *a=dynamic_cast<AclEditorWidget*>(w)) return a; return nullptr; }
static void lost(QDialog *d) { verify(QTest::qWaitFor([&]{return d->property("propertiesTargetUnavailable").toBool();},3000),"event-driven loss transition"); verify(!d->findChild<QLineEdit*>("propertiesName")->isEnabled(),"name disabled after loss"); verify(!d->findChild<QLineEdit*>("propertiesNumericMode")->isEnabled(),"numeric mode disabled after loss"); verify(!d->findChild<QCheckBox*>("propertiesHiddenDotName")->isEnabled(),"hidden disabled after loss"); verify(!d->findChild<QPushButton*>("propertiesXattrAdd")->isEnabled(),"xattr disabled after loss"); verify(!aclWidget(d)->isEnabled(),"ACL disabled after loss"); }
int main(int argc,char **argv) {
 interceptFileJobs=false; interceptPaneRefreshes=true;
 QApplication app(argc,argv); QLocale::setDefault(QLocale(QLocale::English));
 QTemporaryDir tmp(QString::fromLocal8Bit(qgetenv("THISPC_TEST_FILES"))+"/integration-XXXXXX"); verify(tmp.isValid(),"disk fixture");
 QTimer errors; errors.setInterval(5); int boxes=0; QObject::connect(&errors,&QTimer::timeout,[&]{if(auto *b=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { ++boxes; b->close(); }}); errors.start();
 ThisPcWindow window(QUrl::fromLocalFile(tmp.path())); window.show(); window.setSplitViewEnabled(true);
 // Actual shared action routing and KIO writes on each surface, not helper-only parity.
 for(int route=0;route<3;++route) {
  const auto path=make(tmp.filePath(QStringLiteral("route%1.txt").arg(route))); const auto url=QUrl::fromLocalFile(path);
  const bool split=route==1; const auto directory=route==2 ? QUrl("thispcsearch:/fixture") : QUrl::fromLocalFile(tmp.path());
  auto *list=split ? window.m_splitPane->listView() : window.m_directoryList;
  if(split) {window.m_splitPane->setCurrentUrl(directory,false);window.m_splitPane->setViewMode(0);} else {window.m_primaryPane->setCurrentUrl(directory);window.setDirectoryViewMode(0);}
  window.setActivePane(split ? ThisPcWindow::PaneId::Split : ThisPcWindow::PaneId::Primary);list->clear();
  FileInfo info{QFileInfo(path).fileName(),{},{},url,false,7,0}; list->addFileItem(info,QIcon(),"File","7",{},{});
  list->selectionModel()->setCurrentIndex(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
  window.showSelectedProperties();auto *d=PropertiesLifecycle::instance().openDialogs().last();ready(d);
  const auto caps=PropertiesCapabilityResolver::resolve(url);verify(caps.entryIdentity.valid && caps.posixModeReadable==PropertiesCapabilityState::Supported,"route local capability snapshot");
  auto *numeric=d->findChild<QLineEdit*>("propertiesNumericMode");verify(numeric->isEnabled() && numeric->text()=="0644","route numeric initial parity");
  auto *x=d->findChild<PropertiesXattrWidget*>(); const int before=refreshedPanes.size();
  verify(x->mutate(PropertiesXattrOperation::Add,"user.stage6","integration"),"production xattr add");
  verify(QTest::qWaitFor([&]{return refreshedPanes.size()>before && x->findChild<QPushButton*>("propertiesXattrAdd")->isEnabled();},5000),"xattr readback and pane refresh");
  numeric->setText("0750"); d->findChild<QCheckBox*>("propertiesHiddenDotName")->setChecked(true);press(d,QDialogButtonBox::Apply);
  const auto hidden=tmp.filePath("."+QFileInfo(path).fileName());verify(!QFileInfo::exists(path) && mode(hidden)==0750,"one Apply integrates POSIX and Hidden KIO writes");
  ready(d);verify(!d->property("propertiesTargetUnavailable").toBool(),"own rename does not invalidate dialog");
  auto *acl=aclWidget(d);verify(acl->data().path==hidden,"ACL retargeted to renamed item");
  // Old path replacement must not be the destination of a later ACL write.
  make(path); auto aclData=acl->data(); aclData.accessEntries[0].permissions=AclPermissions::fromBits(6); acl->refresh(aclData); // controller production write from retargeted snapshot
  verify(acl->write().success,"ACL write after rename");verify(mode(path)==0644 && mode(hidden)==0650,"ACL touches new path and leaves replacement untouched");
  char value[32]{};verify(::getxattr(QFile::encodeName(hidden),"user.stage6",value,sizeof value)==11 && QByteArray(value,11)=="integration","xattr survives mode/hidden/ACL");
  numeric->setText("0640");press(d,QDialogButtonBox::Cancel);drain();verify(mode(hidden)==0650,"Cancel discards pending mode");
  d=open(window,hidden);ready(d);d->findChild<QLineEdit*>("propertiesNumericMode")->setText("0640");press(d,QDialogButtonBox::Ok);verify(!d->isVisible() && mode(hidden)==0640,"OK writes and closes");drain();
 }
 verify(boxes==0,"route interactions have no errors");
 // Renaming a link must never turn its target into an editable ACL object.
 const auto linkTarget=make(tmp.filePath("acl-link-target"));
 for(bool broken:{false,true}) {
  const auto linkPath=tmp.filePath(broken ? "acl-broken-link" : "acl-live-link");
  const auto renamedLink=linkPath+"-renamed";
  verify(QFile::link(broken ? tmp.filePath("acl-absent") : linkTarget,linkPath),"ACL rename symlink fixture");
  auto *linkDialog=open(window,linkPath); auto *linkAcl=aclWidget(linkDialog);
  verify(linkAcl && linkAcl->data().capability==AclCapability::SymlinkUnavailable,"link ACL initially unavailable");
  linkDialog->findChild<QLineEdit*>("propertiesName")->setText(QFileInfo(renamedLink).fileName());
  press(linkDialog,QDialogButtonBox::Apply);
  verify(QFileInfo(renamedLink).isSymLink() && !QFileInfo(linkPath).isSymLink(),"link itself renamed");
  verify(linkAcl->data().capability==AclCapability::SymlinkUnavailable && !linkAcl->data().canWrite(),"renamed link ACL remains unavailable");
  verify(!linkAcl->write().success,"renamed link ACL write rejected");
  verify(mode(linkTarget)==0644,"link target mode unchanged after rename and denied ACL write");
  linkDialog->close(); drain();
 }
 // Deleted, externally moved, held-original replacement, parent disappearance.
 for(int scenario=0;scenario<4;++scenario) {
  const auto parent=tmp.filePath(QStringLiteral("loss%1").arg(scenario));verify(QDir().mkpath(parent),"loss parent created");const auto path=make(parent+"/target");auto *d=open(window,path);ready(d);
  if(scenario==0) verify(QFile::remove(path),"external delete");
  if(scenario==1) verify(QFile::rename(path,parent+"/moved"),"external move");
  if(scenario==2) {verify(QFile::rename(path,tmp.filePath("held-original")),"hold inode");make(path);}
  if(scenario==3) verify(QDir().rename(parent,parent+"-detached"),"parent detached");
  lost(d);const int b=boxes;press(d,QDialogButtonBox::Apply);verify(boxes>b && d->isVisible(),"lost target gives controlled error");
  if(scenario==2) verify(mode(path)==0644,"replacement mode untouched"); else verify(!QFileInfo::exists(path),"lost target not recreated");
  d->close();drain();
 }
 // Deterministic storage notification reaches the production dialog without hardware removal.
 const auto path=make(tmp.filePath("device-loss"));auto *d=open(window,path);ready(d);
 auto *devices=d->findChild<SolidDeviceMonitor*>("propertiesDeviceMonitor");verify(devices,"dialog observes Solid changes");
 auto *watch=d->findChild<QFileSystemWatcher*>();watch->removePaths(watch->files()+watch->directories());
 verify(QFile::rename(path,path+"-held"),"storage target removed from old location");
 Q_EMIT devices->devicesChanged();lost(d);d->close();drain();
 // Busy Properties main-window close must wait; delivery then closes all dialogs.
 d=open(window,tmp.filePath("device-loss-held"));auto *busy=dynamic_cast<PropertiesWindow*>(d);verify(busy,"production busy window");busy->setBusy(true);QPointer<QDialog> guard=d;
 window.close();verify(!guard.isNull() && window.isVisible(),"parent close deferred while Properties busy");busy->setBusy(false);verify(QTest::qWaitFor([&]{drain();return guard.isNull() && !window.isVisible();},3000),"idle completes deferred app close");
 verify(PropertiesLifecycle::instance().openCount()==0,"no dialogs after app close");
 // A real KIO chmod spins the event loop: close the parent from inside it.
 {
  ThisPcWindow nested(QUrl::fromLocalFile(tmp.path()));nested.show();
  const auto target=make(tmp.filePath("nested-close"));auto *dialog=open(nested,target);ready(dialog);
  dialog->findChild<QLineEdit*>("propertiesNumericMode")->setText("0600");
  QPointer<QDialog> closeGuard=dialog; bool observedBusy=false;
  QTimer::singleShot(0,&nested,[&]{observedBusy=dynamic_cast<PropertiesWindow*>(dialog)->isBusy();nested.close();});
  press(dialog,QDialogButtonBox::Apply);
  verify(QTest::qWaitFor([&]{drain();return closeGuard.isNull() && !nested.isVisible();},3000),"real KIO nested loop parent close completes");
  verify(observedBusy && mode(target)==0600,"parent close happened during real Apply with verified mode");
 }
 // Filesystem resolution on real local fixture, followed by deterministic backend policy.
 auto caps=PropertiesCapabilityResolver::resolve(QUrl::fromLocalFile(tmp.filePath("nested-close")));
 qInfo("Stage6 real fixture filesystem: %s",qPrintable(caps.fileSystemType));
 verify(caps.entryIdentity.valid && caps.userXattrEditable==PropertiesCapabilityState::Supported,"real local capability/xattr resolution");
 KIO::UDSEntry entry;auto visibility=PropertiesHiddenPolicy::fromEntry(caps.requestedUrl,entry);
 for(const char *fs:{"btrfs","ext4"}) {caps.fileSystemType=fs;verify(PropertiesHiddenPolicy::editable(caps.requestedUrl,caps,visibility,"nested-close"),"deterministic supported Unix policy");verify(PropertiesPosixMode::editable(caps.requestedUrl,caps),"Unix numeric capability policy");}
 for(const char *fs:{"ntfs","ntfs3","fuseblk"}) {caps.fileSystemType=fs;verify(!PropertiesHiddenPolicy::editable(caps.requestedUrl,caps,visibility,"nested-close"),"NTFS hidden no dot-name write");verify(PropertiesPosixMode::editable(caps.requestedUrl,caps),"NTFS POSIX capability is not blocked by type alone");}
 caps.posixModeEditable=PropertiesCapabilityState::ReadOnly;verify(!PropertiesPosixMode::editable(caps.requestedUrl,caps),"read-only numeric disabled");
 // Remote calls cannot reach injected local syscalls, including revalidation.
 int calls=0;PropertiesCapabilityResolver::LocalSyscalls sys;sys.lstatFn=[&](const char*,struct stat*){++calls;return -1;};sys.statFn=sys.lstatFn;sys.accessFn=[&](const char*,int){++calls;return -1;};sys.listXattrFn=[&](const char*,char*,size_t)->ssize_t{++calls;return -1;};
 for(const char *scheme:{"sftp","smb","ftp","webdav"}) {const auto c=PropertiesCapabilityResolver::resolve(QUrl(QString::fromLatin1(scheme)+"://host/file"),sys);verify(c.isRemote,"remote backend resolved");verify(PropertiesCapabilityResolver::revalidate(c,sys)==PropertiesRevalidationResult::Unsupported,"remote revalidation unsupported");}verify(calls==0,"remote zero local syscalls");
 qInfo("PASS: %d Properties integration assertions",checks);return 0;
}
