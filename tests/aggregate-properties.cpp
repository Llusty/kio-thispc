#include "aggregatepropertiesscan.h"
#include <QTest>
static int checks = 0;
static void verify(bool value, const char *message) { if (!value) qFatal("FAIL: %s", message); ++checks; }
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    verify(tmp.isValid(), "isolated temporary fixture");
    QDir dir(tmp.path()); dir.mkdir("a"); dir.mkdir("b"); dir.mkdir("a/nested");
    const auto write = [&](const QString &name, const QByteArray &data) { QFile f(tmp.filePath(name)); verify(f.open(QIODevice::WriteOnly), "create file"); verify(f.write(data) == data.size(), "write fixture"); };
    write("a/one", "abc"); write("a/nested/.hidden", "12345"); write("b/two", "12");
    verify(::link(QFile::encodeName(tmp.filePath("a/one")), QFile::encodeName(tmp.filePath("b/hard"))) == 0, "hardlink fixture");
    verify(::symlink("../a", QFile::encodeName(tmp.filePath("b/link"))) == 0, "symlink fixture");
    auto token = std::make_shared<std::atomic_bool>(false);
    const auto snapshot = [&](const QStringList &names) { QList<QUrl> urls; for (const auto &name : names) urls << QUrl::fromLocalFile(tmp.filePath(name)); return AggregateSnapshot::capture(urls); };
    const QLocale pl(QLocale::Polish, QLocale::Poland);
    verify(aggregateSize(8, pl) == "8 B" && aggregateSize(9, pl) == "9 B", "exact small bytes");
    verify(aggregateCount(267498675, pl).contains(pl.groupSeparator()), "grouped Polish counters");
    verify(aggregateSize(267498675, pl) == QStringLiteral("255,11 MiB (%1 bajtów)").arg(aggregateCount(267498675, pl)), "scaled Polish size with exact bytes");
    verify(aggregateCount(4096, pl) == QStringLiteral("4") + pl.groupSeparator() + QStringLiteral("096"), "four-digit Polish counter");
    verify(aggregateCount(8, pl) == "8" && aggregateCount(9, pl) == "9", "small Polish counters unchanged");
    verify(aggregateCount(267498675, pl) == QStringLiteral("267") + pl.groupSeparator() + QStringLiteral("498") + pl.groupSeparator() + QStringLiteral("675"), "exact large Polish grouping");
    verify(aggregateSize(4096, pl) == QStringLiteral("4,00 KiB (4") + pl.groupSeparator() + QStringLiteral("096 bajtów)"), "four-digit exact allocated bytes");
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    verify(aggregateCount(4096, en) == "4,096" && aggregateSize(4096, en) == "4.00 KiB (4,096 bytes)", "English locale unchanged");
    const auto allocation = [&](const QString &name) { struct stat st {}; verify(::lstat(QFile::encodeName(tmp.filePath(name)), &st) == 0, "fixture allocation stat"); return quint64(st.st_blocks) * 512; };
    auto roots = snapshot({"a", "b"});
    auto r = AggregatePropertiesScan::run(roots, token);
    verify(!r.partial() && r.files == 4 && r.folders == 3 && r.links == 1 && r.bytes == 13, "nested hidden symlink and hardlink semantics");
    verify(r.allocatedBytes == allocation("a/one") + allocation("a/nested/.hidden") + allocation("b/two") && r.allocatedFiles == 3, "hardlink allocation once and symlink directory excluded");
    r = AggregatePropertiesScan::run(snapshot({"a/one", "a", "a/nested"}), token);
    verify(r.files == 2 && r.folders == 2 && r.bytes == 8, "overlap with child selected first");
    verify(r.allocatedBytes == allocation("a/one") + allocation("a/nested/.hidden"), "overlap allocation child first");
    r = AggregatePropertiesScan::run(snapshot({"a", "a/one"}), token);
    verify(r.files == 2 && r.bytes == 8, "overlap parent first");
    verify(r.allocatedBytes == allocation("a/one") + allocation("a/nested/.hidden"), "overlap allocation parent first");
    token->store(true); r = AggregatePropertiesScan::run(roots, token);
    verify(r.cancelled && r.files == 0 && r.allocatedBytes == 0, "cancel before start"); token->store(false);
    r = AggregatePropertiesScan::run(roots, token, 2); verify(r.limited && r.partial(), "bounded scan");
    auto gone = snapshot({"b/two"}); QFile::rename(tmp.filePath("b/two"), tmp.filePath("b/held")); write("b/two", "replacement");
    r = AggregatePropertiesScan::run(gone, token); verify(r.invalidated && r.files == 0, "replacement identity rejected");
    QFile::remove(tmp.filePath("b/two")); r = AggregatePropertiesScan::run(gone, token); verify(r.invalidated, "vanished root");
    AggregateSnapshot remote = AggregateSnapshot::capture({QUrl("sftp://host/file"), QUrl("thispc:/drive")});
    r = AggregatePropertiesScan::run(remote, token); verify(r.unsupported == 2 && r.partial(), "remote and virtual unsupported");
    QFile sparse(tmp.filePath("sparse")); verify(sparse.open(QIODevice::WriteOnly) && sparse.resize(1048576), "sparse fixture"); sparse.close();
    r = AggregatePropertiesScan::run(snapshot({"sparse", "b/link"}), token); verify(r.bytes == 1048576 && r.links == 1, "sparse logical size and root link");
    verify(r.allocatedBytes == allocation("sparse") && r.allocatedBytes < r.bytes, "sparse reported allocation without symlink follow");
    dir.mkdir("denied"); write("denied/file", "secret");
    verify(::chmod(QFile::encodeName(tmp.filePath("denied")), 0000) == 0, "deny fixture");
    r = AggregatePropertiesScan::run(snapshot({"denied"}), token);
    if (::geteuid() != 0) verify(r.skipped && r.partial(), "unreadable directory partial");
    ::chmod(QFile::encodeName(tmp.filePath("denied")), 0700);
    dir.mkdir("cancel");
    for (int i = 0; i < 300; ++i) { QFile f(tmp.filePath("cancel/" + QString::number(i))); verify(f.open(QIODevice::WriteOnly) && f.write("x") == 1, "cancel fixture"); }
    r = AggregatePropertiesScan::run(snapshot({"cancel"}), token, 1000000, [&](const AggregateSummary &) { token->store(true); });
    verify(r.cancelled && r.files < 300, "in-flight cancellation at bounded progress boundary");
    verify(r.allocatedBytes == r.files * allocation("cancel/0") && r.partial(), "cancel allocation matches processed files only");
    token->store(false);
    write("one.txt", "abc"); write("two.txt", "12345");
    auto *complete = new AggregatePropertiesDialog({QUrl::fromLocalFile(tmp.filePath("one.txt")), QUrl::fromLocalFile(tmp.filePath("two.txt"))});
    complete->show();
    auto *finish = complete->findChild<QFutureWatcherBase*>();
    for (int i = 0; i < 500 && !finish->isFinished(); ++i) QTest::qWait(10);
    QCoreApplication::processEvents();
    verify(finish->isFinished(), "async scan finishes");
    verify(complete->findChild<QLabel*>("aggregateFiles")->text() == "2"
        && complete->findChild<QLabel*>("aggregateFolders")->text() == "0"
        && complete->findChild<QLabel*>("aggregateBytes")->text() == "8 B", "two files zero folders eight bytes GUI");
    verify(complete->findChild<QLabel*>("aggregateDiskSize")->text() == aggregateSize(allocation("one.txt") + allocation("two.txt")), "GUI reported allocation");
    verify(complete->findChild<QLabel*>("aggregateSummary")->text() == trLocal("Zakończono", "Complete"), "completed status");
    verify(complete->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->isVisible(), "Cancel remains available for pending edits");
    auto *longValue = complete->findChild<QLabel*>("aggregateDiskSize");
    longValue->setText(QString("long policy ").repeated(150));
    complete->resize(480, 460); QCoreApplication::processEvents();
    auto *area = complete->findChild<QScrollArea*>();
    verify(area->verticalScrollBar()->maximum() > 0, "long content scrolls at minimum geometry");
    verify(area->widget()->width() <= area->viewport()->width(), "no horizontal overflow");
    QPointer<QDialog> life(complete); complete->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    verify(life.isNull(), "completed dialog safe close");
    auto *cancelled = new AggregatePropertiesDialog({QUrl::fromLocalFile(tmp.filePath("cancel"))});
    cancelled->show();
    cancelled->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
    auto *cancelFinish = cancelled->findChild<QFutureWatcherBase*>();
    for (int i = 0; i < 500 && !cancelFinish->isFinished(); ++i) QTest::qWait(10);
    QCoreApplication::processEvents();
    verify(cancelFinish->isFinished() && cancelled->isVisible(), "Cancel stops scan without closing dialog");
    const auto cancelResult = static_cast<QFutureWatcher<AggregateSummary>*>(cancelFinish)->result();
    verify(cancelled->findChild<QLabel*>("aggregateSummary")->text().contains(cancelResult.cancelled
        ? trLocal("Anulowano", "Cancelled") : trLocal("Zakończono", "Complete")),
        "Cancel reports actual outcome even if scan finished before click");
    verify(cancelled->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->isHidden(), "Cancel absent after stopped scan");
    cancelled->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ThisPcWindow window(QUrl::fromLocalFile(tmp.path()), false);
    window.show(); window.setSplitViewEnabled(true);
    for (int route = 0; route < 3; ++route) {
        const bool split = route == 1;
        auto *list = split ? window.m_splitPane->listView() : window.m_directoryList;
        const QUrl directory = route == 2 ? QUrl("thispcsearch:/fixture") : QUrl::fromLocalFile(tmp.path());
        if (split) { window.m_splitPane->setCurrentUrl(directory, false); window.m_splitPane->setViewMode(0); }
        else { window.m_primaryPane->setCurrentUrl(directory); window.setDirectoryViewMode(0); }
        window.setActivePane(split ? ThisPcWindow::PaneId::Split : ThisPcWindow::PaneId::Primary);
        list->clear();
        for (const auto &name : {QString("a/one"), QString("sparse")}) {
            FileInfo info{QFileInfo(name).fileName(), {}, {}, QUrl::fromLocalFile(tmp.filePath(name)), false, 3, 0};
            list->addFileItem(info, QIcon(), "File", "3", {}, {});
        }
        list->selectionModel()->select(list->model()->index(0,0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        list->selectionModel()->select(list->model()->index(1,0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        window.updateFileActionStates();
        verify(window.m_propertiesAction->isEnabled(), "multi action enabled in Primary Split Search");
        const auto before = window.paneContext().directory;
        window.activateWindow(); list->setFocus(); QCoreApplication::processEvents();
        QTest::keyClick(list, Qt::Key_Return, Qt::AltModifier);
        AggregatePropertiesDialog *dialog = nullptr;
        for (auto *child : window.findChildren<QDialog*>()) if (auto *d = dynamic_cast<AggregatePropertiesDialog*>(child)) dialog = d;
        verify(dialog && !dialog->isModal(), "Alt Enter opens modeless aggregate dialog");
        verify(window.paneContext().directory == before, "Alt Enter does not navigate");
        verify(dialog->findChildren<QLineEdit*>().isEmpty(), "no single-item editing controls");
        auto *tabs = dialog->findChild<QTabWidget*>();
        verify(tabs && tabs->count() == 2 && tabs->tabText(0) == trLocal("Ogólne", "General"), "General and Permissions tabs");
        verify(dialog->size() == QSize(630, 680) && dialog->minimumSize() == QSize(480, 460), "shared geometry");
        auto *header = dialog->findChild<QLabel*>("propertiesHeaderName");
        verify(header && header->font().bold() && header->wordWrap(), "shared styled wrapping header");
        verify(dialog->findChild<QLabel*>("propertiesHeaderIcon"), "shared icon");
        auto *scroll = dialog->findChild<QScrollArea*>();
        verify(scroll && scroll->widgetResizable() && scroll->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff, "responsive scroll shell");
        auto *buttons = dialog->findChild<QDialogButtonBox*>();
        verify(buttons && buttons->button(QDialogButtonBox::Ok) && buttons->button(QDialogButtonBox::Apply), "standard OK and guarded Apply");
        auto *choice = dialog->findChild<QComboBox*>("bulkChange8");
        for (int i = 0; i < 500 && !choice->isEnabled(); ++i) QTest::qWait(10);
        verify(choice->isEnabled(), "Permissions editing enabled after scan completes");
        if (route == 2) {
            verify(QFile::rename(tmp.filePath("a/one"), tmp.filePath("a/moved")), "move selected root while dialog open");
            auto *devices = dialog->findChild<SolidDeviceMonitor*>();
            verify(devices, "aggregate observes storage changes");
            Q_EMIT devices->devicesChanged();
            verify(dialog->findChild<QLabel*>("aggregateSummary")->text().contains(trLocal("Wynik częściowy", "Partial results")), "storage notification invalidates lost selected root");
            verify(!choice->isEnabled(), "Permissions editing disabled on invalidation");
        }
        dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    // Integration test: mixed selection (file + dir + symlink) in Primary View
    {
        auto *list = window.m_directoryList;
        window.setActivePane(ThisPcWindow::PaneId::Primary);
        window.m_primaryPane->setCurrentUrl(QUrl::fromLocalFile(tmp.path()));
        list->clear();
        FileInfo fInfo{QString("a/nested/.hidden"), {}, {}, QUrl::fromLocalFile(tmp.filePath("a/nested/.hidden")), false, 5, 0};
        list->addFileItem(fInfo, QIcon(), "File", "5", {}, {});
        FileInfo dInfo{QString("b"), {}, {}, QUrl::fromLocalFile(tmp.filePath("b")), true, 0, 0};
        list->addFileItem(dInfo, QIcon(), "Folder", "0", {}, {});
        FileInfo lInfo{QString("b/link"), {}, {}, QUrl::fromLocalFile(tmp.filePath("b/link")), false, 0, 0};
        list->addFileItem(lInfo, QIcon(), "Link", "0", {}, {});
        for (int i = 0; i < 3; ++i) {
            list->selectionModel()->select(list->model()->index(i, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        window.updateFileActionStates();
        verify(window.m_propertiesAction->isEnabled(), "multi-selection with symlink action enabled");
        list->setFocus(); QCoreApplication::processEvents();
        QTest::keyClick(list, Qt::Key_Return, Qt::AltModifier);
        AggregatePropertiesDialog *mixedDialog = nullptr;
        for (auto *child : window.findChildren<QDialog*>()) if (auto *d = dynamic_cast<AggregatePropertiesDialog*>(child)) mixedDialog = d;
        verify(mixedDialog != nullptr, "modeless dialog opened for mixed selection");
        auto *finish = mixedDialog->findChild<QFutureWatcherBase*>();
        for (int i = 0; i < 500 && !finish->isFinished(); ++i) QTest::qWait(10);
        QCoreApplication::processEvents();
        auto *permWidget = mixedDialog->findChild<QWidget*>("aggregatePermissions");
        verify(permWidget != nullptr, "Permissions tab exists in mixed selection");
        auto *choiceMixed = mixedDialog->findChild<QComboBox*>("bulkChange8");
        verify(choiceMixed != nullptr && !choiceMixed->isEnabled(), "mixed selection containing symlink has disabled choices");
        auto *permStatus = mixedDialog->findChild<QLabel*>("bulkPermissionStatus");
        verify(permStatus && permStatus->text().contains(trLocal("tylko do odczytu", "read-only")), "status reflects read-only policy due to symlink");
        mixedDialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    qInfo("PASS: %d aggregate Properties checks", checks);
}
