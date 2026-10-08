#include "propertiesxattrwidget.h"
#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QSemaphore>
#include <QTest>
#include <atomic>
static int checks=0;
static void verify(bool ok,const char *why) { if(!ok) qFatal("FAIL: %s",why); ++checks; }
using S=PropertiesXattrStatus;
using T=PropertiesXattrType;
struct FakeXattrs {
    QMap<QByteArray,QByteArray> values;
    int calls=0, listError=0, getError=0, listRanges=0, getRanges=0, identities=0;
    bool replaced=false, missing=false, missingAfter=false;
    mode_t mode=S_IFREG;
    QByteArray denied;
    qint64 oversized=0;
    PropertiesXattrReader::Backend backend() {
        PropertiesXattrReader::Backend b;
        b.identity=[this](const char *,struct stat *s) { ++calls; ++identities; if(missing || (missingAfter && identities>1)) { errno=ENOENT; return -1; } s->st_dev=1; s->st_ino=replaced && identities>1 ? 2 : 1; s->st_mode=mode; return 0; };
        b.list=[this](const char *,char *buf,size_t size)->ssize_t {
            ++calls; if(listError) { errno=listError; return -1; }
            QByteArray names; for(auto it=values.cend();it!=values.cbegin();) { --it; names+=it.key(); names+='\0'; }
            if(!buf) return names.size();
            if(listRanges-->0 || size<size_t(names.size())) { errno=ERANGE; return -1; }
            memcpy(buf,names.data(),names.size()); return names.size();
        };
        b.get=[this](const char *,const char *name,void *buf,size_t size)->ssize_t {
            ++calls; if(getError || denied==name) { errno=getError ? getError : EACCES; return -1; }
            if(oversized) return oversized;
            const auto value=values.value(name);
            if(!buf) return value.size();
            if(getRanges-->0 || size<size_t(value.size())) { errno=ERANGE; return -1; }
            memcpy(buf,value.data(),value.size()); return value.size();
        };
        return b;
    }
    PropertiesXattrSnapshot read() { return PropertiesXattrReader::read(QUrl::fromLocalFile("/fake"),{},backend()); }
};
static void drain(int ms=60) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    FakeXattrs f;
    verify(f.read().status==S::Empty,"supported empty");
    for(int e : {ENOTSUP,EACCES,EPERM,ENOENT,EIO}) {
        f.listError=e; verify(f.read().status==PropertiesXattrReader::errorStatus(e),"list errno mapping");
    }
    f.listError=0;
    const QVector<QByteArray> inputs={"hello",QString::fromUtf8("Zażółć gęślą jaźń").toUtf8(),"one\ntwo\r\tthree",QByteArray("a\0b",3),QByteArray(1,'\1'),QByteArray::fromHex("c080"),QByteArray::fromHex("00ff107f"),QByteArray()};
    for(int i=0;i<inputs.size();++i) {
        f.values={{"user.test",inputs[i]}}; const auto s=f.read(); const auto e=s.entries.first();
        verify(s.status==S::Ready && e.byteSize==inputs[i].size(),"value exists and full byte count");
        verify(e.raw==inputs[i],"exact bytes including empty value");
        verify(e.type==((i<3 || i==7) ? T::Text : T::Binary),"conservative text/binary classification");
        if(e.type==T::Binary) verify(e.preview==QString::fromLatin1(inputs[i].toHex(' ')),"deterministic hex");
        else verify(e.preview==QString::fromUtf8(inputs[i]),"Unicode and whitespace preserved");
    }
    f.values={{"user.z","z"},{"security.fake",QByteArray::fromHex("ff")},{"trusted.fake","t"},{"system.fake","s"}};
    auto s=f.read(); verify(s.entries[0].name=="security.fake" && s.entries.last().name=="user.z","deterministic namespace ordering");
    const auto oldValues=f.values; f.read(); verify(f.values==oldValues,"read-only backend has no mutation interface");
    f.denied="trusted.fake"; s=f.read(); verify(s.status==S::Ready && s.entries[2].status==S::PermissionDenied && s.entries[3].raw=="z","per-entry denial preserves others");
    f.denied.clear(); f.getError=EIO; verify(f.read().entries.first().status==S::Error,"per-entry generic error"); f.getError=0;
    f.values={{"user.test","hello"}};
    f.listRanges=1; verify(f.read().status==S::Ready,"list size race retries");
    f.listRanges=10; int before=f.calls; verify(f.read().status==S::Error && f.calls-before<=7,"list retry bounded"); f.listRanges=0;
    f.getRanges=1; verify(f.read().entries[0].raw=="hello","value size race retries");
    f.getRanges=10; verify(f.read().entries[0].status==S::Error,"value retry bounded"); f.getRanges=0;
    f.oversized=1000000; s=f.read(); verify(s.entries[0].byteSize==1000000 && s.entries[0].truncated && s.entries[0].raw.isEmpty() && s.entries[0].type==T::TooLarge,"oversize size-only no allocation"); f.oversized=0;
    f.values={{"user.long",QByteArray(3000,'a')}}; s=f.read(); verify(s.entries[0].type==T::Text && s.entries[0].preview.size()==1024 && s.entries[0].truncated,"bounded text preview");
    f.values={{"user.tail",QByteArray(2000,'a')+QByteArray::fromHex("ff")}};
    verify(f.read().entries[0].type==T::Binary,"classification checks full value beyond preview");
    f.values={{"user.binary",QByteArray(200,'\0')}}; s=f.read(); verify(s.entries[0].preview==QString::fromLatin1(QByteArray(64,'\0').toHex(' ')) && s.entries[0].truncated,"bounded binary preview");
    f.values.clear(); for(int i=0;i<6;++i) f.values[QByteArray::number(i)]=QByteArray(65536,'x');
    s=f.read(); qsizetype bytes=0; for(const auto &e:s.entries) bytes+=e.raw.size(); verify(bytes==262144 && s.entries.last().type==T::TooLarge,"total raw budget bounded");
    f.values.clear(); for(int i=0;i<300;++i) f.values[QByteArray::number(i)]="v"; s=f.read(); verify(s.entries.size()==256 && s.truncated,"entry limit explicit");
    f.values={{"user.link","link"}}; f.mode=S_IFLNK; verify(f.read().entries[0].name=="user.link","fake symlink contract reads link attributes only");
    f.mode=S_IFREG; f.missing=true; verify(f.read().status==S::Missing,"missing entry stable"); f.missing=false;
    f.identities=0; f.missingAfter=true; s=f.read();
    verify(s.status==S::Missing && s.entries.isEmpty(),"target disappears during read: discard entries"); f.missingAfter=false;
    f.identities=0; f.replaced=true; s=f.read(); verify(s.status==S::Replaced && s.entries.isEmpty(),"replacement discards snapshot"); f.replaced=false;
    verify(PropertiesXattrReader::read(QUrl::fromLocalFile("/fake"),{true,1,999,S_IFREG},f.backend()).status==S::Replaced,"initial identity mismatch ignored");
    for(const char *scheme : {"sftp","smb","fish","ftp","webdav","webdavs","http","https","admin"}) {
        before=f.calls; s=PropertiesXattrReader::read(QUrl(QString::fromLatin1(scheme)+"://host/a"),{},f.backend());
        verify(s.status==S::Unsupported && before==f.calls,"remote zero local backend calls");
    }
    QTemporaryDir tmp; verify(tmp.isValid(),"temporary real syscall fixture");
    const QString path=tmp.filePath("target"); QFile file(path); verify(file.open(QIODevice::WriteOnly),"fixture opened"); file.write("bytes"); file.close();
    const QByteArray p=QFile::encodeName(path);
    const QString link=tmp.filePath("link"),broken=tmp.filePath("broken");
    verify(::symlink(p.constData(),QFile::encodeName(link).constData())==0,"real symlink");
    verify(::symlink("missing",QFile::encodeName(broken).constData())==0,"real broken link");
    bool realXattr=false;
    if(::setxattr(p.constData(),"user.thispc.target","target",6,0)==0) {
        realXattr=true;
        struct stat st1{},st2{}; ::lstat(p.constData(),&st1);
        const auto original=PropertiesXattrReader::read(QUrl::fromLocalFile(path));
        verify(original.status==S::Ready,"real syscall xattr read");
        s=PropertiesXattrReader::read(QUrl::fromLocalFile(link));
        bool leaked=false; for(const auto &e:s.entries) leaked|=e.name=="user.thispc.target";
        verify(!leaked,"real link never reads target attribute");
        verify(PropertiesXattrReader::read(QUrl::fromLocalFile(broken)).status!=S::Missing,"broken link exists and no follow");
        const auto after=PropertiesXattrReader::read(QUrl::fromLocalFile(path)); ::lstat(p.constData(),&st2);
        verify(original.entries.size()==after.entries.size() && original.entries[0].raw==after.entries[0].raw,"real attrs unchanged");
        verify(st1.st_mode==st2.st_mode && st1.st_mtim.tv_sec==st2.st_mtim.tv_sec && st1.st_mtim.tv_nsec==st2.st_mtim.tv_nsec,"mode mtime unchanged");
        verify(file.open(QIODevice::ReadOnly) && file.readAll()=="bytes","file bytes unchanged"); file.close();
    } else qInfo("SKIP: real FS xattr unsupported/denied: %d",errno);
    std::atomic<int> calls{0};
    PropertiesXattrProvider provider(nullptr,[&](const QUrl &u,const PropertiesObjectIdentity &) { ++calls; QThread::msleep(u.path()=="/old"?40:1); PropertiesXattrSnapshot r; r.status=u.path()=="/old"?S::Error:S::Empty; return r; });
    int delivered=0; S last=S::Unknown;
    QObject::connect(&provider,&PropertiesXattrProvider::ready,&app,[&](const auto &r){ ++delivered; last=r.status; });
    provider.load(QUrl::fromLocalFile("/old")); provider.load(QUrl::fromLocalFile("/new")); drain(100);
    verify(delivered==1 && last==S::Empty && calls==2,"generation suppresses old asynchronous result");
    provider.load(QUrl::fromLocalFile("/old")); provider.cancel(); drain(100); verify(delivered==1,"canceled request ignored");
    auto *closing=new PropertiesXattrProvider(nullptr,[](const QUrl &,const PropertiesObjectIdentity &){ QThread::msleep(30); return PropertiesXattrSnapshot{}; });
    closing->load(QUrl::fromLocalFile(path)); delete closing; drain(100); verify(true,"close during read safe");
    QLocale::setDefault(QLocale(QLocale::Polish)); verify(PropertiesXattrWidget::statusText(S::Empty)=="Brak atrybutów rozszerzonych","Polish label");
    QLocale::setDefault(QLocale(QLocale::English)); verify(PropertiesXattrWidget::statusText(S::Empty)=="No extended attributes","English label");
    PropertiesXattrWidget widget(QUrl::fromLocalFile(path)); auto *table=widget.findChild<QTableWidget*>();
    verify(table && table->editTriggers()==QAbstractItemView::NoEditTriggers,"UI read-only table");
    verify(!widget.findChild<QPushButton*>("propertiesXattrAdd")->isEnabled(),"no mutation enabled without captured identity");
    for (const auto &bytes : {QByteArray::fromHex("c3"), QByteArray::fromHex("e282"), QByteArray::fromHex("c285")}) {
        PropertiesXattrEntry e; e.raw=bytes; PropertiesXattrReader::present(e);
        verify(e.type==T::Binary,"incomplete UTF8 and C1 controls are binary");
    }
    interceptFileJobs=false;
    ThisPcWindow window(QUrl::fromLocalFile(tmp.path()));
    window.setSplitViewEnabled(true);
    drain(100);
    for(int route=0;route<3;++route) {
        const bool split=route==1;
        const QUrl directory=route==2 ? QUrl(QStringLiteral("thispcsearch:/fixture")) : QUrl::fromLocalFile(tmp.path());
        auto *list=split ? window.m_splitPane->listView() : window.m_directoryList;
        if(split) { window.m_splitPane->setCurrentUrl(directory,false); window.m_splitPane->setViewMode(0); }
        else { window.m_primaryPane->setCurrentUrl(directory); window.setDirectoryViewMode(0); }
        window.setActivePane(split ? ThisPcWindow::PaneId::Split : ThisPcWindow::PaneId::Primary);
        list->clear();
        FileInfo info{QStringLiteral("target"),QString(),QString(),QUrl::fromLocalFile(path),false,5,0};
        list->addFileItem(info,QIcon(),QStringLiteral("File"),QStringLiteral("5"),QString(),QString());
        list->selectionModel()->setCurrentIndex(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        window.showSelectedProperties();
        const auto dialogs=PropertiesLifecycle::instance().openDialogs();
        verify(!dialogs.isEmpty(),"Primary Split Search opens real Properties");
        auto *dialog=dialogs.last();
        auto *xattr=dialog->findChild<PropertiesXattrWidget*>();
        verify(xattr!=nullptr,"Primary Split Search shares xattr viewer");
        xattr->activate();
        verify(QTest::qWaitFor([&] { return realXattr ? xattr->findChild<QTableWidget*>()->rowCount()>0 : !xattr->findChild<QLabel*>(QStringLiteral("propertiesXattrStatus"))->text().contains(QStringLiteral("Loading")); },5000),"Primary Split Search xattr rows arrive");
        dialog->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    window.close();
    qInfo("PASS: %d properties_xattrs assertions",checks); return 0;
}
