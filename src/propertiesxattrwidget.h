#pragma once
#include "browsercommon.h"
#include "propertiesxattrwriter.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QMessageBox>
#include <QPointer>
#include <QLabel>
#include <QHeaderView>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QVBoxLayout>
class PropertiesXattrWidget final : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesXattrWidget(const QUrl &url, QWidget *parent = nullptr, PropertiesObjectIdentity identity = {}, PropertiesTargetCapabilities capabilities = {}, PropertiesXattrMutationController::Writer writer = {}, PropertiesXattrProvider::Reader reader = {}) : QWidget(parent), m_url(url), m_identity(identity), m_capabilities(capabilities) {
        m_capabilities.requestedUrl=url; m_capabilities.normalizedUrl=url;
        if (!capabilities.entryIdentity.valid) {
            m_capabilities.entryIdentity=identity; m_capabilities.isLocal=url.isLocalFile(); m_capabilities.isRemote=!url.isLocalFile();
            m_capabilities.userXattrEditable=PropertiesCapabilityState::Unsupported;
        }
        setObjectName("propertiesXattrs");
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel(trLocal("Atrybuty rozszerzone", "Extended Attributes"),this));
        m_status = new QLabel(trLocal("Odczyt po otwarciu karty.", "Loaded when the tab is opened."),this);
        m_status->setObjectName(QStringLiteral("propertiesXattrStatus"));
        m_status->setWordWrap(true); layout->addWidget(m_status);
        m_table = new QTableWidget(0,4,this);
        m_table->setObjectName("propertiesXattrTable");
        m_table->setHorizontalHeaderLabels({trLocal("Nazwa","Name"),trLocal("Wartość","Value"),trLocal("Typ","Type"),trLocal("Rozmiar","Size")});
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        m_table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
        m_table->setColumnWidth(0,180); m_table->setColumnWidth(2,90); m_table->setColumnWidth(3,90);
        m_table->setTextElideMode(Qt::ElideRight); m_table->setWordWrap(false);
        m_table->setMinimumWidth(0); m_table->setMaximumHeight(240); m_table->hide(); layout->addWidget(m_table);
        m_detail = new QPlainTextEdit(this);
        m_detail->setReadOnly(true); m_detail->setMaximumHeight(90); m_detail->hide();
        layout->addWidget(m_detail);
        connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
            const auto *value = m_table->item(row, 1);
            m_detail->setPlainText(value ? value->text() : QString());
        });
        auto *actions=new QHBoxLayout;
        m_add=new QPushButton(trLocal("Dodaj","Add"),this); m_add->setObjectName("propertiesXattrAdd");
        m_edit=new QPushButton(trLocal("Edytuj","Edit"),this); m_edit->setObjectName("propertiesXattrEdit");
        m_remove=new QPushButton(trLocal("Usuń","Remove"),this); m_remove->setObjectName("propertiesXattrRemove");
        for(auto *button:{m_add,m_edit,m_remove}) actions->addWidget(button);
        actions->addStretch(); layout->addLayout(actions);
        m_error=new QLabel(this); m_error->setObjectName("propertiesXattrWriteError"); m_error->setWordWrap(true); m_error->hide(); layout->addWidget(m_error);
        layout->addWidget(new QLabel(trLocal("Zmiany xattr są zapisywane od razu, poza Zastosuj i Cofnij.","Attribute changes are saved immediately, outside Apply and Undo."),this));
        m_controller=new PropertiesXattrMutationController(this,std::move(writer));
        connect(m_controller,&PropertiesXattrMutationController::busyChanged,this,[this] { updateButtons(); });
        connect(m_controller,&PropertiesXattrMutationController::completed,this,[this](const auto &result) {
            m_writeError=result.status==PropertiesXattrWriteStatus::Success ? QString() : writeStatusText(result.status);
            m_error->setText(m_writeError); m_error->setVisible(!m_writeError.isEmpty());
            reload(); // Never patch cached rows, including failure/conflict results.
            if(result.status==PropertiesXattrWriteStatus::Success) Q_EMIT attributesChanged();
        });
        connect(m_add,&QPushButton::clicked,this,[this] { openEditor(nullptr); });
        connect(m_edit,&QPushButton::clicked,this,[this] { const auto *entry=selected(); if(entry && m_edit->isEnabled()) openEditor(entry); });
        connect(m_remove,&QPushButton::clicked,this,[this] {
            const auto *entry=selected(); if(!entry || !m_remove->isEnabled()) return;
            const auto name=entry->name; const auto generation=m_targetGeneration; QPointer<PropertiesXattrWidget> alive(this);
            const auto answer=QMessageBox::question(this,trLocal("Usuń atrybut","Remove attribute"),trLocal("Usunąć atrybut %1? Zmiana jest natychmiastowa.","Remove attribute %1? This change is immediate.").arg(QString::fromUtf8(name)),QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
            if(alive && generation==m_targetGeneration && answer==QMessageBox::Yes) mutate(PropertiesXattrOperation::Remove,name);
        });
        connect(m_table,&QTableWidget::currentCellChanged,this,[this] { updateButtons(); });
        m_provider = new PropertiesXattrProvider(this,std::move(reader));
        updateButtons();
        connect(m_provider,&PropertiesXattrProvider::ready,this,&PropertiesXattrWidget::render);
    }
    static QString statusText(PropertiesXattrStatus s) {
        switch(s) {
        case PropertiesXattrStatus::Empty: return trLocal("Brak atrybutów rozszerzonych", "No extended attributes");
        case PropertiesXattrStatus::Unsupported: return trLocal("Atrybuty rozszerzone nie są obsługiwane dla tego elementu/backendu", "Extended attributes are unsupported for this item/backend");
        case PropertiesXattrStatus::PermissionDenied: return trLocal("Brak uprawnień do odczytu atrybutów rozszerzonych", "Permission denied reading extended attributes");
        case PropertiesXattrStatus::Missing: return trLocal("Element zniknął podczas odczytu.", "The item disappeared during reading.");
        case PropertiesXattrStatus::Replaced: return trLocal("Element został podmieniony; wynik pominięto.", "The item was replaced; the result was discarded.");
        case PropertiesXattrStatus::Error: return trLocal("Nie udało się odczytać atrybutów rozszerzonych.", "Could not read extended attributes.");
        case PropertiesXattrStatus::Unknown: return trLocal("Atrybuty rozszerzone są niedostępne.", "Extended attributes are unavailable.");
        case PropertiesXattrStatus::Ready: return {};
        }
        return {};
    }
    void activate() { if (m_started) return; m_started = true; m_status->setText(trLocal("Odczytywanie atrybutów…", "Loading attributes…")); m_provider->load(m_url,m_identity); }
    void setUrl(const QUrl &url) { m_provider->cancel(); m_controller->invalidate(); ++m_targetGeneration; m_loaded=false; m_loading=false; m_snapshot={}; m_writeError.clear(); m_error->clear(); m_error->hide(); m_capabilities.requestedUrl=url; m_capabilities.normalizedUrl=url; m_capabilities.isLocal=url.isLocalFile(); m_capabilities.isRemote=!url.isLocalFile(); m_url=url; m_started=false; m_table->setRowCount(0); m_table->hide(); m_detail->hide(); m_detail->clear(); m_status->setText(trLocal("Odczyt po otwarciu karty.","Loaded when the tab is opened.")); updateButtons(); if(isVisible()) activate(); }
    static QString writeStatusText(PropertiesXattrWriteStatus status) {
        using S=PropertiesXattrWriteStatus;
        switch(status) {
        case S::Success: return {};
        case S::InvalidName: return trLocal("Nazwa musi być poprawnym UTF-8, zaczynać się od user., mieć niepustą końcówkę, bez NUL, do 255 bajtów.","Name must be valid UTF-8, start with user., have a nonempty suffix, contain no NUL, and fit in 255 bytes.");
        case S::TooLarge: return trLocal("Nazwa lub wartość przekracza limit systemu plików (wartość maks. 65536 bajtów).","Name or value exceeds the filesystem limit (value maximum 65536 bytes).");
        case S::Unsupported: return trLocal("Edycja xattr nie jest obsługiwana dla tego elementu/backendu.","Attribute editing is unsupported for this item/backend.");
        case S::PermissionDenied: return trLocal("Brak uprawnień do zmiany atrybutu.","Permission denied changing the attribute.");
        case S::ReadOnly: return trLocal("System plików jest tylko do odczytu.","The filesystem is read-only.");
        case S::NoSpace: return trLocal("Brak miejsca lub przekroczony limit dyskowy.","Insufficient space or disk quota exceeded.");
        case S::ConflictExists: return trLocal("Atrybut już istnieje; nie został nadpisany.","The attribute already exists; it was not overwritten.");
        case S::ConflictMissing: return trLocal("Atrybut już nie istnieje; nie został utworzony ponownie.","The attribute no longer exists; it was not recreated.");
        case S::Missing: return trLocal("Element zniknął; zapis zablokowany.","The item disappeared; writing was blocked.");
        case S::Replaced: return trLocal("Element został podmieniony; zapis zablokowany. Otwórz Właściwości ponownie.","The item was replaced; writing was blocked. Reopen Properties.");
        case S::Unknown: return trLocal("Nie można potwierdzić tożsamości elementu; zapis zablokowany.","The item identity could not be verified; writing was blocked.");
        case S::Error: return trLocal("Nie udało się zmienić atrybutu (błąd wejścia/wyjścia).","Could not change the attribute (input/output error).");
        }
        return {};
    }
    void invalidateTarget() {
        m_provider->cancel(); m_controller->invalidate(); ++m_targetGeneration;
        m_capabilities.userXattrEditable=PropertiesCapabilityState::Unsupported;
        m_loading=false;
        m_status->setText(trLocal("Element niedostępny lub podmieniony. Otwórz Właściwości ponownie.", "Item unavailable or replaced. Reopen Properties."));
        updateButtons();
    }
Q_SIGNALS:
    void attributesChanged();
public:
    bool mutate(PropertiesXattrOperation operation,const QByteArray &name,const QByteArray &value={}) {
        if(!canWrite() || !PropertiesXattrWriter::validName(name) || m_controller->busy()) return false;
        if(operation!=PropertiesXattrOperation::Add) {
            bool exists=false; for(const auto &entry:m_snapshot.entries) if(entry.name==name && entry.status==PropertiesXattrStatus::Ready) exists=true;
            if(!exists) return false;
        }
        m_keepSelection=name; m_provider->cancel();
        return m_controller->submit(m_capabilities,operation,name,value);
    }
private:
    bool canWrite() const {
        return m_loaded && !m_loading && m_url.isLocalFile() && m_identity.valid
            && (m_identity.type==S_IFREG || m_identity.type==S_IFDIR)
            && m_capabilities.userXattrEditable==PropertiesCapabilityState::Supported
            && (m_snapshot.status==PropertiesXattrStatus::Ready || m_snapshot.status==PropertiesXattrStatus::Empty);
    }
    const PropertiesXattrEntry *selected() const {
        const int row=m_table->currentRow(); return row>=0 && row<m_snapshot.entries.size() ? &m_snapshot.entries[row] : nullptr;
    }
    void updateButtons() {
        const bool enabled=canWrite() && !m_controller->busy(); const auto *e=selected();
        const bool editable=enabled && e && e->status==PropertiesXattrStatus::Ready && PropertiesXattrWriter::validName(e->name);
        m_add->setEnabled(enabled); m_edit->setEnabled(editable && e->type!=PropertiesXattrType::TooLarge); m_remove->setEnabled(editable);
    }
    void reload() { m_loading=true; updateButtons(); m_provider->load(m_url,m_identity); }
    void openEditor(const PropertiesXattrEntry *entry) {
        if(!canWrite() || m_controller->busy()) return;
        const bool edit=entry!=nullptr;
        const QByteArray originalName=edit ? entry->name : QByteArray();
        const QByteArray originalRaw=edit ? entry->raw : QByteArray();
        const quint64 generation=m_targetGeneration;
        QPointer<PropertiesXattrWidget> alive(this);
        QPointer<QDialog> dialog=new QDialog(this); dialog->setWindowTitle(edit ? trLocal("Edytuj atrybut","Edit attribute") : trLocal("Dodaj atrybut","Add attribute"));
        auto *layout=new QVBoxLayout(dialog); auto *form=new QFormLayout;
        auto *name=new QLineEdit(edit ? QString::fromUtf8(originalName) : QStringLiteral("user."),dialog); name->setObjectName("propertiesXattrEditorName"); name->setReadOnly(edit);
        auto *format=new QComboBox(dialog); format->setObjectName("propertiesXattrEditorFormat"); format->addItems({trLocal("Tekst UTF-8","UTF-8 text"),trLocal("Hex: pary cyfr, opcjonalne spacje","Hex: digit pairs, optional spaces")});
        auto *value=new QPlainTextEdit(dialog); value->setObjectName("propertiesXattrEditorValue"); value->setMaximumBlockCount(0);
        if(edit) { const bool binary=entry->type==PropertiesXattrType::Binary; format->setCurrentIndex(binary ? 1 : 0); value->setPlainText(binary ? QString::fromLatin1(entry->raw.toHex(' ')) : QString::fromUtf8(entry->raw)); format->setEnabled(false); }
        // Qt normalizes paragraph separators on display. Preserve original bytes
        // when an existing text value is saved without changing its contents.
        const QString initialText=value->toPlainText();
        form->addRow(trLocal("Nazwa:","Name:"),name); form->addRow(trLocal("Format:","Format:"),format); form->addRow(trLocal("Wartość:","Value:"),value); layout->addLayout(form);
        auto *error=new QLabel(dialog); error->setObjectName("propertiesXattrEditorError"); error->setWordWrap(true); layout->addWidget(error);
        layout->addWidget(new QLabel(trLocal("Zapis nastąpi natychmiast. Brak Cofnij dla xattr.","Saving is immediate. Attributes have no Undo."),dialog));
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,dialog); layout->addWidget(buttons);
        QByteArray parsed;
        connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);
        connect(buttons,&QDialogButtonBox::accepted,dialog,[=,&parsed] {
            if(!PropertiesXattrWriter::validName(name->text().toUtf8()) || QString::fromUtf8(name->text().toUtf8())!=name->text()) { error->setText(writeStatusText(PropertiesXattrWriteStatus::InvalidName)); return; }
            const bool ok=format->currentIndex()==1 ? PropertiesXattrWriter::parseHex(value->toPlainText(),&parsed) : PropertiesXattrWriter::encodeText(value->toPlainText(),&parsed);
            if(!ok) { error->setText(trLocal("Nieprawidłowa wartość lub przekroczony limit 65536 bajtów. Hex wymaga pełnych par cyfr i tylko spacji.","Invalid value or 65536-byte limit exceeded. Hex requires complete digit pairs and spaces only.")); return; }
            if(edit && format->currentIndex()==0 && value->toPlainText()==initialText) parsed=originalRaw;
            dialog->accept();
        });
        const int result=dialog->exec();
        if(alive && dialog && result==QDialog::Accepted && generation==m_targetGeneration) mutate(edit ? PropertiesXattrOperation::Edit : PropertiesXattrOperation::Add,name->text().toUtf8(),parsed);
        if(dialog) dialog->deleteLater();
    }
    void render(const PropertiesXattrSnapshot &s) {
        m_snapshot=s; m_loaded=true; m_loading=false;
        m_detail->clear(); m_detail->setVisible(!s.entries.isEmpty());
        m_table->setRowCount(s.entries.size()); m_table->setVisible(!s.entries.isEmpty());
        m_status->setText(s.truncated ? trLocal("Lista skrócona do 256 atrybutów.","List limited to 256 attributes.") : statusText(s.status));
        for(qsizetype i=0;i<s.entries.size();++i) {
            const auto &e=s.entries[i];
            QString value = e.status == PropertiesXattrStatus::Ready ? e.preview : statusText(e.status);
            if(e.truncated) value += trLocal(" … [podgląd skrócony]", " … [preview truncated]");
            const QString type = e.type==PropertiesXattrType::Text ? trLocal("Tekst","Text") : e.type==PropertiesXattrType::Binary ? trLocal("Binarne","Binary") : trLocal("Za duże","Too large");
            const QStringList cells{QString::fromUtf8(e.name),value,type,(e.byteSize < 0 ? trLocal("Niedostępny","Unavailable") : QString::number(e.byteSize)+QStringLiteral(" B"))};
            for(int c=0;c<4;++c) {
                auto *item=new QTableWidgetItem(cells[c]); item->setFlags(item->flags() & ~Qt::ItemIsEditable); item->setToolTip(cells[c].left(256).toHtmlEscaped().replace(QLatin1Char('\n'),QStringLiteral(" "))); m_table->setItem(i,c,item);
            }
        }
        if (!s.entries.isEmpty()) {
            int selectedRow=0; for(int i=0;i<s.entries.size();++i) if(s.entries[i].name==m_keepSelection) selectedRow=i;
            m_table->setCurrentCell(selectedRow,1);
        }
        updateButtons();
    }
    QUrl m_url; PropertiesObjectIdentity m_identity; bool m_started=false;
    PropertiesTargetCapabilities m_capabilities; PropertiesXattrSnapshot m_snapshot;
    PropertiesXattrMutationController *m_controller; QPushButton *m_add,*m_edit,*m_remove; QLabel *m_error;
    bool m_loaded=false,m_loading=false; QByteArray m_keepSelection; QString m_writeError; quint64 m_targetGeneration=0;
    QPlainTextEdit *m_detail; QLabel *m_status; QTableWidget *m_table; PropertiesXattrProvider *m_provider;
};
