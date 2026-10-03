/* Deterministic routing regression for the Access and Default ACL editor tables. */
#include "acleditorwidget.h"
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

static int aclEditorChecks = 0;
static void aclEditorVerify(bool value, const char *description) { if (!value) qFatal("FAIL: %s", description); ++aclEditorChecks; }

static bool hasEntry(const QList<AclEntryData> &entries, AclTag tag, uint qualifier = 0)
{
    for (const auto &entry : entries)
        if (entry.tag == tag && (!entry.isNamed() || entry.qualifier == qualifier)) return true;
    return false;
}

static int namedRow(QTableWidget *table, AclTag tag, uint qualifier)
{
    for (int row = 0; row < table->rowCount(); ++row)
        if (AclTag(table->item(row, 0)->data(Qt::UserRole).toInt()) == tag
            && table->item(row, 0)->data(Qt::UserRole + 1).toUInt() == qualifier) return row;
    return -1;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temp; aclEditorVerify(temp.isValid(), "temporary directory");
    const QString directory = temp.filePath("folder"); aclEditorVerify(QDir().mkpath(directory), "directory fixture");
    AclData directoryAcl = AclProvider::load(directory, true, false, true, false);
    directoryAcl.defaultEntries = AclController::minimalFromMode(0750);
    AclEditorWidget editor(directoryAcl);
    editor.show(); QApplication::processEvents();
    auto *access = editor.accessTableForTest(); auto *defaults = editor.defaultTableForTest();
    aclEditorVerify(access && defaults, "directory exposes access and default tables");

    QEvent accessFocus(QEvent::FocusIn); QApplication::sendEvent(access, &accessFocus);
    aclEditorVerify(editor.addNamedForTest(AclTag::NamedUser, 65533), "AddUser routed from access focus");
    aclEditorVerify(hasEntry(editor.data().accessEntries, AclTag::NamedUser, 65533), "access receives named user");
    aclEditorVerify(!hasEntry(editor.data().defaultEntries, AclTag::NamedUser, 65533), "access AddUser leaves default unchanged");

    QTest::mouseClick(defaults->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(2, defaults->viewport()->height() - 2));
    aclEditorVerify(editor.addNamedForTest(AclTag::NamedUser, 65534), "AddUser routed from default viewport");
    aclEditorVerify(hasEntry(editor.data().defaultEntries, AclTag::NamedUser, 65534), "default receives named user");
    aclEditorVerify(hasEntry(editor.data().defaultEntries, AclTag::Mask), "default named user creates mask");
    aclEditorVerify(!hasEntry(editor.data().accessEntries, AclTag::NamedUser, 65534), "default AddUser leaves access unchanged");

    QTest::mouseClick(access->horizontalHeader()->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(2, 2));
    aclEditorVerify(editor.addNamedForTest(AclTag::NamedGroup, 65533), "AddGroup routed from access header");
    aclEditorVerify(hasEntry(editor.data().accessEntries, AclTag::NamedGroup, 65533), "access receives named group");
    aclEditorVerify(!hasEntry(editor.data().defaultEntries, AclTag::NamedGroup, 65533), "access AddGroup leaves default unchanged");

    defaults->setCurrentCell(namedRow(defaults, AclTag::NamedUser, 65534), 0);
    aclEditorVerify(editor.addNamedForTest(AclTag::NamedGroup, 65534), "AddGroup routed from default selection");
    aclEditorVerify(hasEntry(editor.data().defaultEntries, AclTag::NamedGroup, 65534), "default receives named group");
    aclEditorVerify(!hasEntry(editor.data().accessEntries, AclTag::NamedGroup, 65534), "default AddGroup leaves access unchanged");

    access->setCurrentCell(namedRow(access, AclTag::NamedGroup, 65533), 0); editor.removeSelectedForTest();
    aclEditorVerify(!hasEntry(editor.data().accessEntries, AclTag::NamedGroup, 65533), "RemoveEntry removes access selection");
    aclEditorVerify(hasEntry(editor.data().defaultEntries, AclTag::NamedGroup, 65534), "access removal leaves default intact");
    defaults->setCurrentCell(namedRow(defaults, AclTag::NamedGroup, 65534), 0); editor.removeSelectedForTest();
    aclEditorVerify(!hasEntry(editor.data().defaultEntries, AclTag::NamedGroup, 65534), "RemoveEntry removes default selection");
    aclEditorVerify(hasEntry(editor.data().accessEntries, AclTag::NamedUser, 65533), "default removal leaves access intact");

    QString error; aclEditorVerify(editor.prepareForWrite(0750, &error), "routed ACL tables validate");
    const auto saved = editor.write(); aclEditorVerify(saved.success, "controller writes routed ACL data");
    const AclData reread = AclProvider::load(directory, true, false, true, false);
    aclEditorVerify(hasEntry(reread.defaultEntries, AclTag::NamedUser, 65534) && hasEntry(reread.defaultEntries, AclTag::Mask), "default named user and mask survive reread");
    aclEditorVerify(hasEntry(reread.accessEntries, AclTag::NamedUser, 65533), "existing access ACL survives default write");

    editor.toggleDefaultForTest();
    aclEditorVerify(editor.data().defaultEntries.isEmpty(), "Delete default ACL clears only default data");
    aclEditorVerify(hasEntry(editor.data().accessEntries, AclTag::NamedUser, 65533), "Delete default ACL preserves access data");

    const QString filePath = temp.filePath("file"); QFile file(filePath); aclEditorVerify(file.open(QIODevice::WriteOnly), "file fixture"); file.close();
    AclEditorWidget fileEditor(AclProvider::load(filePath, true, false, false, false));
    aclEditorVerify(fileEditor.defaultTableForTest() == nullptr, "file has no default ACL section");
    fileEditor.toggleDefaultForTest();
    aclEditorVerify(fileEditor.data().defaultEntries.isEmpty(), "file remains unchanged without default section");
    qInfo("PASS: %d ACL editor routing assertions; focus, viewport, header, selection, add/remove, controller reread", aclEditorChecks);
}
