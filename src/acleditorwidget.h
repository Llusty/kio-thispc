/* Small presentation/editor layer for POSIX ACLs in PropertiesDialog. */
#pragma once

#include "aclcontroller.h"
#include "browsercommon.h"

#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>
#include <climits>
#include <functional>

class AclEditorWidget : public QGroupBox
{
public:
    explicit AclEditorWidget(const AclData &data, QWidget *parent = nullptr)
        : QGroupBox(trLocal("Listy kontroli dostępu (ACL)", "Access control lists (ACL)"), parent), m_data(data)
    {
        auto *layout = new QVBoxLayout(this);
        m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
        m_access = makeTable(trLocal("ACL dostępu", "Access ACL"), layout);
        if (data.isDirectory) m_default = makeTable(trLocal("Domyślna ACL (tylko nowe elementy)", "Default ACL (new children only)"), layout);
        auto *buttons = new QHBoxLayout;
        m_addUser = new QPushButton(trLocal("Dodaj użytkownika…", "Add user…"), this);
        m_addGroup = new QPushButton(trLocal("Dodaj grupę…", "Add group…"), this);
        m_remove = new QPushButton(trLocal("Usuń wpis", "Remove entry"), this);
        buttons->addWidget(m_addUser); buttons->addWidget(m_addGroup); buttons->addWidget(m_remove);
        if (data.isDirectory) { m_toggleDefault = new QPushButton(this); buttons->addWidget(m_toggleDefault); }
        buttons->addStretch(); layout->addLayout(buttons);
        connect(m_addUser, &QPushButton::clicked, this, [this] { addNamed(AclTag::NamedUser); });
        connect(m_addGroup, &QPushButton::clicked, this, [this] { addNamed(AclTag::NamedGroup); });
        connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(); });
        if (m_toggleDefault) connect(m_toggleDefault, &QPushButton::clicked, this, [this] { toggleDefault(); });
        populate(); setCapabilityState();
    }

    void refresh(const AclData &data) {
        m_data = data; m_dirty = false; m_explicitAccessEdits.clear();
        populate(); setCapabilityState(); emitDirty();
    }

    bool isDirty() const { return m_dirty; }
    const AclData &data() const { return m_data; }
    void setDirtyChangedCallback(std::function<void(bool)> callback) { m_dirtyChanged = std::move(callback); }

#ifdef THISPC_TEST_HARNESS
    QTableWidget *accessTableForTest() const { return m_access; }
    QTableWidget *defaultTableForTest() const { return m_default; }
    bool addNamedForTest(AclTag tag, uint id) { return addNamedWithId(tag, id); }
    void removeSelectedForTest() { removeSelected(); }
    void toggleDefaultForTest() { toggleDefault(); }
#endif

    bool prepareForWrite(int mode, QString *error)
    {
        if (!readTable(m_access, &m_data.accessEntries, error)) return false;
        if (m_default && !readTable(m_default, &m_data.defaultEntries, error)) return false;
        m_data.hasExtendedAccess = AclProvider::hasExtended(m_data.accessEntries);
        for (auto &entry : m_data.accessEntries) {
            if (m_explicitAccessEdits.contains(entry.tag)) continue;
            if (entry.tag == AclTag::UserObject) entry.permissions = AclPermissions::fromBits((mode >> 6) & 7);
            if (entry.tag == AclTag::GroupObject && !m_data.hasExtendedAccess) entry.permissions = AclPermissions::fromBits((mode >> 3) & 7);
            if (entry.tag == AclTag::Mask && m_data.hasExtendedAccess) entry.permissions = AclPermissions::fromBits((mode >> 3) & 7);
            if (entry.tag == AclTag::Other) entry.permissions = AclPermissions::fromBits(mode & 7);
        }
        AclProvider::applyEffective(m_data.accessEntries);
        return AclController::validateEntries(m_data.accessEntries, false, error)
            && AclController::validateEntries(m_data.defaultEntries, true, error);
    }

    AclController::Result write()
    {
        auto result = AclController::write(m_data);
        if (result.success) { m_data = result.data; m_dirty = false; m_explicitAccessEdits.clear(); populate(); emitDirty(); }
        return result;
    }

private:
    enum class AclSection { Access, Default };

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress) {
            if (belongsTo(m_access, watched)) setActiveSection(AclSection::Access);
            else if (belongsTo(m_default, watched)) setActiveSection(AclSection::Default);
        }
        return QGroupBox::eventFilter(watched, event);
    }

    static bool belongsTo(QTableWidget *table, QObject *object)
    {
        return table && (object == table || object == table->viewport() || object == table->horizontalHeader()
                         || object == table->horizontalHeader()->viewport());
    }

    void setActiveSection(AclSection section)
    {
        if (section == AclSection::Default && !m_default) return;
        m_activeSection = section;
    }

    QTableWidget *activeTable() const
    {
        return m_activeSection == AclSection::Default && m_default ? m_default : m_access;
    }

    QTableWidget *makeTable(const QString &title, QVBoxLayout *layout)
    {
        layout->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(title), this));
        auto *table = new QTableWidget(this); table->setColumnCount(3);
        table->setHorizontalHeaderLabels({trLocal("Wpis", "Entry"), trLocal("Zapisane", "Stored"), trLocal("Efektywne", "Effective")});
        table->horizontalHeader()->setStretchLastSection(true); table->setSelectionBehavior(QAbstractItemView::SelectRows); layout->addWidget(table);
        table->installEventFilter(this); table->viewport()->installEventFilter(this); table->horizontalHeader()->installEventFilter(this); table->horizontalHeader()->viewport()->installEventFilter(this);
        connect(table, &QTableWidget::currentCellChanged, this, [this, table](int, int, int, int) {
            setActiveSection(table == m_default ? AclSection::Default : AclSection::Access);
        });
        connect(table, &QTableWidget::itemChanged, this, [this, table](QTableWidgetItem *item) {
            if (m_populating || item->column() != 1) return;
            if (table == m_access && table->item(item->row(), 0))
                m_explicitAccessEdits.insert(AclTag(table->item(item->row(), 0)->data(Qt::UserRole).toInt()));
            markDirty();
        });
        return table;
    }
    static QString perms(const AclPermissions &p) { return QStringLiteral("%1%2%3").arg(p.read ? 'r' : '-').arg(p.write ? 'w' : '-').arg(p.execute ? 'x' : '-'); }
    static bool parsePerms(const QString &text, AclPermissions *out)
    {
        const QString value = text.trimmed(); if (value.size() != 3) return false;
        if (!QStringLiteral("r-").contains(value[0]) || !QStringLiteral("w-").contains(value[1]) || !QStringLiteral("x-").contains(value[2])) return false;
        *out = AclPermissions{value[0] == QLatin1Char('r'), value[1] == QLatin1Char('w'), value[2] == QLatin1Char('x')}; return true;
    }
    static QString tagLabel(const AclEntryData &entry)
    {
        switch (entry.tag) {
        case AclTag::UserObject: return trLocal("Właściciel", "Owner");
        case AclTag::NamedUser: return trLocal("Użytkownik: ", "User: ") + entry.name + QStringLiteral(" (%1)").arg(entry.qualifier);
        case AclTag::GroupObject: return trLocal("Grupa właściciela", "Owning group");
        case AclTag::NamedGroup: return trLocal("Grupa: ", "Group: ") + entry.name + QStringLiteral(" (%1)").arg(entry.qualifier);
        case AclTag::Mask: return trLocal("Maska", "Mask");
        case AclTag::Other: return trLocal("Inni", "Other");
        } return {};
    }
    void fill(QTableWidget *table, const QList<AclEntryData> &entries)
    {
        table->setRowCount(entries.size());
        for (int row = 0; row < entries.size(); ++row) {
            const auto &entry = entries[row]; auto *label = new QTableWidgetItem(tagLabel(entry));
            label->setData(Qt::UserRole, int(entry.tag)); label->setData(Qt::UserRole + 1, entry.qualifier); label->setFlags(label->flags() & ~Qt::ItemIsEditable);
            auto *stored = new QTableWidgetItem(perms(entry.permissions)); auto *effective = new QTableWidgetItem(perms(entry.effectivePermissions)); effective->setFlags(effective->flags() & ~Qt::ItemIsEditable);
            table->setItem(row, 0, label); table->setItem(row, 1, stored); table->setItem(row, 2, effective);
        }
    }
    void populate() { m_populating = true; fill(m_access, m_data.accessEntries); if (m_default) fill(m_default, m_data.defaultEntries); if (m_toggleDefault) m_toggleDefault->setText(m_data.defaultEntries.isEmpty() ? trLocal("Utwórz domyślną ACL", "Create default ACL") : trLocal("Usuń domyślną ACL", "Delete default ACL")); m_populating = false; }
    bool readTable(QTableWidget *table, QList<AclEntryData> *entries, QString *error)
    {
        if (!table) { entries->clear(); return true; } QList<AclEntryData> result;
        for (int row = 0; row < table->rowCount(); ++row) {
            AclEntryData entry; entry.tag = AclTag(table->item(row, 0)->data(Qt::UserRole).toInt()); entry.qualifier = table->item(row, 0)->data(Qt::UserRole + 1).toUInt();
            entry.name = entry.tag == AclTag::NamedUser ? AclProvider::userName(entry.qualifier) : entry.tag == AclTag::NamedGroup ? AclProvider::groupName(entry.qualifier) : QString();
            if (!parsePerms(table->item(row, 1)->text(), &entry.permissions)) { if (error) *error = trLocal("Prawa muszą mieć format rwx, np. rw-.", "Permissions must use rwx form, for example rw-."); return false; }
            result << entry;
        }
        AclProvider::applyEffective(result); *entries = result; return true;
    }
    void addNamed(AclTag tag)
    {
        bool ok = false; const int id = QInputDialog::getInt(this, tag == AclTag::NamedUser ? trLocal("Użytkownik ACL", "ACL user") : trLocal("Grupa ACL", "ACL group"), trLocal("Identyfikator UID/GID:", "UID/GID number:"), 0, 0, INT_MAX, 1, &ok); if (!ok) return;
        addNamedWithId(tag, uint(id));
    }
    bool addNamedWithId(AclTag tag, uint id)
    {
        QTableWidget *table = activeTable();
        QList<AclEntryData> entries; QString error; if (!readTable(table, &entries, &error)) return false;
        for (const auto &e : entries) if (e.tag == tag && e.qualifier == id) return false;
        AclEntryData named; named.tag = tag; named.qualifier = id; named.name = tag == AclTag::NamedUser ? AclProvider::userName(id) : AclProvider::groupName(id); named.permissions = AclPermissions::fromBits(4); entries << named;
        bool hasMask = false; for (const auto &e : entries) hasMask |= e.tag == AclTag::Mask;
        if (!hasMask) { AclEntryData mask; mask.tag = AclTag::Mask; mask.permissions = AclPermissions::fromBits(7); entries << mask; }
        if (table == m_default) {
            m_data.defaultEntries = entries; AclProvider::applyEffective(m_data.defaultEntries);
        } else {
            m_data.accessEntries = entries; m_data.hasExtendedAccess = true; AclProvider::applyEffective(m_data.accessEntries);
        }
        populate(); markDirty(); return true;
    }
    void removeSelected()
    {
        QTableWidget *table = activeTable();
        const int row = table->currentRow(); if (row < 0) return; const AclTag tag = AclTag(table->item(row, 0)->data(Qt::UserRole).toInt());
        if (tag != AclTag::NamedUser && tag != AclTag::NamedGroup) return;
        const uint id = table->item(row, 0)->data(Qt::UserRole + 1).toUInt();
        QList<AclEntryData> &entries = table == m_default ? m_data.defaultEntries : m_data.accessEntries;
        AclController::removeNamed(entries, tag, id);
        if (table == m_access) m_data.hasExtendedAccess = AclProvider::hasExtended(m_data.accessEntries);
        populate(); markDirty();
    }
    void toggleDefault()
    {
        if (!m_default) return;
        if (m_data.defaultEntries.isEmpty()) {
            int mode = 0; for (const auto &entry : m_data.accessEntries) { if (entry.tag == AclTag::UserObject) mode |= entry.permissions.bits() << 6; if (entry.tag == AclTag::GroupObject) mode |= entry.permissions.bits() << 3; if (entry.tag == AclTag::Other) mode |= entry.permissions.bits(); }
            m_data.defaultEntries = AclController::minimalFromMode(mode);
        } else m_data.defaultEntries.clear();
        populate(); markDirty();
    }
    void markDirty() { if (!m_dirty) { m_dirty = true; emitDirty(); } }
    void emitDirty() { if (m_dirtyChanged) m_dirtyChanged(m_dirty); }
    void setCapabilityState()
    {
        const bool readable = m_data.canRead(), writable = m_data.canWrite();
        setEnabled(readable); m_access->setEnabled(writable); if (m_default) m_default->setEnabled(writable); m_addUser->setEnabled(writable); m_addGroup->setEnabled(writable); m_remove->setEnabled(writable);
        if (readable) m_status->setText(m_data.hasExtendedAccess ? trLocal("Rozszerzona ACL. Efektywne prawa uwzględniają maskę.", "Extended ACL. Effective permissions include the mask.") : trLocal("Minimalna ACL (klasyczne owner/group/other).", "Minimal ACL (classic owner/group/other)."));
        else m_status->setText(m_data.errorMessage);
    }
    AclData m_data; QLabel *m_status = nullptr; QTableWidget *m_access = nullptr; QTableWidget *m_default = nullptr;
    QPushButton *m_addUser = nullptr; QPushButton *m_addGroup = nullptr; QPushButton *m_remove = nullptr; QPushButton *m_toggleDefault = nullptr;
    bool m_dirty = false; bool m_populating = false; std::function<void(bool)> m_dirtyChanged;
    AclSection m_activeSection = AclSection::Access;
    QSet<AclTag> m_explicitAccessEdits;
};
