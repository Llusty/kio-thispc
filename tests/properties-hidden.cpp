// Production dialog/KIO writes use disposable fixtures only.
#include "propertieshidden.h"
#include <QTest>
static int checks = 0;
static void verify(bool ok, const char *why) { if (!ok) qFatal("FAIL: %s", why); ++checks; }
static void drain() { QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); }
int main(int argc, char **argv) {
    QApplication app(argc,argv);
    QLocale::setDefault(QLocale(QLocale::English));
    QTemporaryDir temp; verify(temp.isValid(),"disposable fixture");
    auto make = [&](const QString &name) {
        const auto path=temp.filePath(name); QFile file(path);
        verify(file.open(QIODevice::WriteOnly) && file.write("unchanged bytes")==15,"fixture content"); file.close();
        return path;
    };
    const auto plain=make("plain.txt"), hidden=make(".hidden.txt");
    KIO::UDSEntry entry;
    for(const char *scheme:{"file","sftp","smb","ftp","webdav","admin","trash","thispc"}) {
        for(const char *name:{"plain",".dot","..dots"}) {
            QUrl url(QString::fromLatin1(scheme)+":///"+QString::fromLatin1(name));
            auto s=PropertiesHiddenPolicy::fromEntry(url,entry);
            verify(s.ready && !s.kioOverride && s.hidden==PropertiesHiddenPolicy::dotHidden(name),"KIO name fallback");
            for(int flag:{0,1}) {
                KIO::UDSEntry e; e.fastInsert(KIO::UDSEntry::UDS_HIDDEN,flag);
                s=PropertiesHiddenPolicy::fromEntry(url,e);
                verify(s.kioOverride && s.hidden==bool(flag),"explicit UDS_HIDDEN including zero overrides dot");
            }
        }
    }
    verify(!PropertiesHiddenPolicy::dotHidden(".") && !PropertiesHiddenPolicy::dotHidden(""),"current directory and empty name not hidden");
    const QUrl url=QUrl::fromLocalFile(plain);
    auto caps=PropertiesCapabilityResolver::resolve(url);
    auto state=PropertiesHiddenPolicy::fromEntry(url,entry);
    for(const char *fs:{"ext4","btrfs","xfs","tmpfs"}) {
        caps.fileSystemType=fs;
        verify(PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"recognized Unix dot-name edit");
    }
    for(const char *fs:{"ntfs","ntfs3","fuseblk","vfat","exfat","cifs","nfs","fuse",""}) {
        caps.fileSystemType=fs;
        verify(!PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"native/unknown/network mounts read-only");
    }
    caps=PropertiesCapabilityResolver::resolve(url);
    verify(PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"real fixture filesystem supports safe rename");
    for(const char *name:{".","..","..dots","","a/b"," spaced "})
        verify(!PropertiesHiddenPolicy::editable(url,caps,state,name),"ambiguous/invalid names disabled");
    state.kioOverride=true; verify(!PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"override read-only even false"); state.kioOverride=false;
    state.ready=false; verify(!PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"unknown state disabled"); state.ready=true;
    caps.entryIdentity.valid=false; verify(!PropertiesHiddenPolicy::editable(url,caps,state,"plain.txt"),"missing identity disabled");
    caps=PropertiesCapabilityResolver::resolve(url);
    for(const char *scheme:{"sftp","smb","ftp","webdav","admin","trash","thispc"})
        verify(!PropertiesHiddenPolicy::editable(QUrl(QString::fromLatin1(scheme)+"://host/a"),caps,state,"a"),"remote even with local-derived caps disabled");
    verify(PropertiesHiddenPolicy::toggledName("żółty.txt",true)==QString::fromUtf8(".żółty.txt"),"hide preserves Unicode/extension");
    verify(PropertiesHiddenPolicy::toggledName(".a.txt",false)=="a.txt","unhide removes one dot");
    int recordings=0, refreshes=0;
    auto open=[&](const QString &path,bool dir=false) {
        auto *d=PropertiesDialog::show(nullptr,QFileInfo(path).fileName(),QUrl::fromLocalFile(path),dir,
            {},{},{},[]{ qFatal("unexpected elevation"); return false; },[&](KIO::CopyJob *){++recordings;},
            {},[&]{++refreshes;});
        auto *w=d->findChild<PropertiesHiddenWidget*>(); verify(w!=nullptr,"real dialog contains hidden module");
        verify(QTest::qWaitFor([&]{ return w->loaded(); },5000),"actual KIO hidden listing arrives");
        return d;
    };
    auto geometry = [&](QDialog *d) {
        auto *w=d->findChild<PropertiesHiddenWidget*>();
        auto *outer=qobject_cast<QVBoxLayout*>(w->parentWidget()->layout());
        verify(outer!=nullptr,"production General vertical layout");
        auto *form=qobject_cast<QFormLayout*>(outer->itemAt(2)->layout());
        verify(form!=nullptr,"production General details form");
        for(int width:{520,760,520}) {
            d->resize(width,480); QTest::qWait(30);
            const int required=w->layout()->heightForWidth(w->width());
            if(w->height()<required) qWarning("hidden height=%d required=%d width=%d",w->height(),required,w->width());
            verify(w->height()>=required,"Hidden receives full height for actual width");
            auto *next=form->itemAt(0,QFormLayout::FieldRole)->widget();
            if(w->geometry().bottom()>=next->geometry().top()) qWarning("Hidden rect=%d,%d,%d,%d next y=%d",w->x(),w->y(),w->width(),w->height(),next->y());
            verify(w->geometry().bottom()<next->geometry().top(),"Hidden does not overlap Type row");
            auto *scroll=qobject_cast<QScrollArea*>(w->parentWidget()->parentWidget()->parentWidget());
            verify(scroll && scroll->verticalScrollBar()->maximum()>0,
                   "General scrolls vertically when its content exceeds the viewport");
        }
    };
    for(auto locale:{QLocale::English,QLocale::Polish}) {
        QLocale::setDefault(QLocale(locale));
        for(int repeat=0;repeat<5;++repeat) {
            auto *dialog=open(plain); geometry(dialog);
            auto *w=dialog->findChild<PropertiesHiddenWidget*>();
            w->reload(url,PropertiesCapabilityResolver::resolve(url)); geometry(dialog);
            verify(QTest::qWaitFor([&]{return w->loaded();},5000),"reload completes"); geometry(dialog);
            dialog->close(); drain();
        }
    }
    QLocale::setDefault(QLocale(QLocale::English));
    auto box=[](QDialog *d){return d->findChild<QCheckBox*>("propertiesHiddenDotName");};
    auto label=[](QDialog *d){return d->findChild<QLabel*>("propertiesHiddenState");};
    auto press=[](QDialog *d,QDialogButtonBox::StandardButton b){d->findChild<QDialogButtonBox*>()->button(b)->click();};
    auto close=[](QDialog *d){d->close();drain();};
    // Measure actual wrapped text in the production General tab, in both languages.
    const auto ambiguous = make("..ambiguous.txt");
    for (auto language : {QLocale::English, QLocale::Polish}) {
        QLocale::setDefault(QLocale(language));
        for (const auto &path : {plain, hidden, ambiguous}) {
            auto *dialog = open(path);
            auto *widget = dialog->findChild<PropertiesHiddenWidget*>();
            verify(box(dialog)->text() == (language == QLocale::Polish ? QStringLiteral("Ukryj") : QStringLiteral("Hide")),
                   "user-facing Hide label in PL and EN");
            for (int width : {630, 500}) {
                dialog->resize(width, 680);
                QTest::qWait(30);
                const auto labels = widget->findChildren<QLabel*>();
                for (auto *text : labels) {
                    verify(widget->rect().contains(text->geometry()), "hidden text fits its enclosing widget");
                    if (text->wordWrap())
                        verify(text->height() >= text->heightForWidth(text->width()), "wrapped hidden text has sufficient height");
                    for (auto *other : labels)
                        if (text != other)
                            verify(!text->geometry().intersects(other->geometry()), "hidden rows do not overlap");
                    verify(!text->geometry().intersects(box(dialog)->geometry()), "hidden text does not overlap checkbox");
                }
                auto *general = widget->parentWidget();
                for (auto *other : general->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
                    if (other != widget && other->isVisible())
                        verify(!widget->geometry().intersects(other->geometry()), "hidden block does not overlap other General rows");
            }
            close(dialog);
        }
    }
    QLocale::setDefault(QLocale(QLocale::English));
    auto *d=open(plain);
    verify(box(d)->isEnabled() && !box(d)->isChecked() && label(d)->text().startsWith("No"),"ordinary actual state editable");
    box(d)->click();
    verify(d->findChild<QLineEdit*>("propertiesName")->text()==".plain.txt" && QFile::exists(plain),"toggle pending only changes Name");
    verify(label(d)->text().startsWith("No"),"actual saved state stays distinct from pending dot");
    press(d,QDialogButtonBox::Cancel);drain();
    verify(QFile::exists(plain) && !QFile::exists(temp.filePath(".plain.txt")),"Cancel no rename");
    struct stat before{},after{}; ::lstat(QFile::encodeName(plain),&before);
    d=open(plain);box(d)->click();press(d,QDialogButtonBox::Apply);
    const auto renamed=temp.filePath(".plain.txt");
    verify(!QFile::exists(plain) && QFile::exists(renamed) && recordings==1,"Apply shared KIO rename and Undo recorder");
    verify(QTest::qWaitFor([&]{return d->findChild<PropertiesHiddenWidget*>()->loaded();},5000) && label(d)->text().startsWith("Yes"),"saved hidden state refreshes");
    geometry(d); // Actual Apply + Hidden rename + asynchronous reload.
    ::lstat(QFile::encodeName(renamed),&after);
    verify(before.st_ino==after.st_ino && before.st_dev==after.st_dev && before.st_mode==after.st_mode && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"rename preserves identity mode mtime");
    box(d)->click();press(d,QDialogButtonBox::Ok);drain();
    verify(QFile::exists(plain) && !QFile::exists(renamed) && recordings==2,"OK unhides through same shared rename");
    d=open(hidden); verify(box(d)->isChecked() && label(d)->text().startsWith("Yes"),"real dot-hidden readback"); close(d);
    const auto directory=temp.filePath("directory"); verify(QDir().mkdir(directory),"directory fixture");
    d=open(directory,true);box(d)->click();press(d,QDialogButtonBox::Apply);
    verify(QDir(temp.filePath(".directory")).exists(),"directory hide shared KIO rename");close(d);
    for(const auto &name:{QStringLiteral("link"),QStringLiteral(".link"),QStringLiteral("broken")}) {
        const auto link=temp.filePath(name);verify(QFile::link(name=="broken"?temp.filePath("absent"):hidden,link),"symlink fixture");
        d=open(link);verify(!box(d)->isEnabled() && box(d)->isChecked()==name.startsWith('.'),"link entry read-only, target not followed for dot-name");close(d);
    }
    // Close controlled error messages, without authorizing a conflict overwrite.
    int messages=0;
    QTimer dismiss; dismiss.setInterval(10);
    QObject::connect(&dismiss,&QTimer::timeout,[&]{if(auto *m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){++messages;m->accept();}});dismiss.start();
    const auto collision=make("collision"),destination=make(".collision");
    d=open(collision);box(d)->click();const int recorded=recordings;press(d,QDialogButtonBox::Apply);
    verify(QFile::exists(collision) && QFile::exists(destination) && recordings==recorded && messages>0,"existing destination blocks before rename");close(d);
    const auto brokenCollision=make("broken-collision");verify(QFile::link(temp.filePath("absent"),temp.filePath(".broken-collision")),"broken destination fixture");
    d=open(brokenCollision);box(d)->click();press(d,QDialogButtonBox::Apply);
    verify(QFile::exists(brokenCollision) && QFileInfo(temp.filePath(".broken-collision")).isSymLink() && recordings==recorded,"broken symlink collision protected");close(d);
    const auto replace=make("replace");d=open(replace);box(d)->click();
    verify(QFile::rename(replace,temp.filePath("held-original")),"hold old inode");make("replace");press(d,QDialogButtonBox::Apply);
    verify(QFile::exists(replace) && !QFile::exists(temp.filePath(".replace")) && recordings==recorded,"replacement identity guard before hidden write");close(d);
    const auto missing=make("missing");d=open(missing);box(d)->click();verify(QFile::rename(missing,temp.filePath("held-missing")),"move fixture out of path");press(d,QDialogButtonBox::Apply);
    verify(!QFile::exists(temp.filePath(".missing")) && recordings==recorded,"missing source blocks hidden write");close(d);
    dismiss.stop();
    QFile contents(plain);verify(contents.open(QIODevice::ReadOnly) && contents.readAll()=="unchanged bytes","content remains unchanged");
    // In-flight jobs cannot deliver into destroyed UI or a changed target.
    auto *provider=new PropertiesHiddenProvider;
    int deliveries=0;
    QObject::connect(provider,&PropertiesHiddenProvider::ready,[&](const auto &){++deliveries;});
    provider->load(url,PropertiesCapabilityResolver::resolve(url));provider->cancel();
    QTest::qWait(80);verify(deliveries==0,"cancel suppresses stale delivery");
    provider->load(url,PropertiesCapabilityResolver::resolve(url));delete provider;QTest::qWait(80);verify(deliveries==0,"destroy during KIO read safe");
    // Real production routing from Primary, Split and Search opens identical widget.
    interceptFileJobs=false;ThisPcWindow window(QUrl::fromLocalFile(temp.path()));window.setSplitViewEnabled(true);
    for(int route=0;route<3;++route) {
        const bool split=route==1;
        const auto folder=route==2?QUrl("thispcsearch:/fixture"):QUrl::fromLocalFile(temp.path());
        auto *list=split?window.m_splitPane->listView():window.m_directoryList;
        if(split){window.m_splitPane->setCurrentUrl(folder,false);window.m_splitPane->setViewMode(0);}
        else{window.m_primaryPane->setCurrentUrl(folder);window.setDirectoryViewMode(0);}
        window.setActivePane(split?ThisPcWindow::PaneId::Split:ThisPcWindow::PaneId::Primary);list->clear();
        FileInfo info{QStringLiteral("plain.txt"),QString(),QString(),url,false,15,0};
        list->addFileItem(info,QIcon(),QStringLiteral("File"),QStringLiteral("15"),QString(),QString());
        list->selectionModel()->setCurrentIndex(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
        window.showSelectedProperties();const auto dialogs=PropertiesLifecycle::instance().openDialogs();
        verify(!dialogs.isEmpty(),"production pane/search opens Properties");auto *dialog=dialogs.last();
        auto *w=dialog->findChild<PropertiesHiddenWidget*>();const bool loaded=w && QTest::qWaitFor([&]{return w->loaded();},5000);
        if(!loaded) qWarning("route=%d title=%s state=%s",route,qPrintable(dialog->windowTitle()),qPrintable(label(dialog)?label(dialog)->text():QStringLiteral("missing widget")));
        verify(loaded,"pane/search same KIO hidden provider");
        verify(label(dialog)->text().startsWith("No") && box(dialog)->isEnabled(),"Primary/Split/Search hidden parity");close(dialog);
    }
    window.close();
    qInfo("PASS: %d Hidden semantics assertions",checks);
    return 0;
}
