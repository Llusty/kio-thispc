/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "selectionmenucontroller.h"

#include "directoryviewsettings.h"

#include "browsercommon.h"
#include "directoryviewsettings.h"

#include <KFileItem>
#include <KFileItemActions>
#include <KFileItemListProperties>

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QStandardPaths>
#include <QTextDocument>
#include <QWidget>

SelectionMenuController::SelectionMenuController(QWidget *parent) : m_parent(parent) {}

void SelectionMenuController::addOpenWithSubmenu(QMenu &menu, const QList<QUrl> &urls) const
{
    if (urls.isEmpty()) return;
    QMenu *submenu = menu.addMenu(themedIcon(QStringLiteral("document-open-with"), QStringLiteral("system-run")),
                                  trLocal("Otwórz za pomocą", "Open with"));
    KFileItemList items;
    items.reserve(urls.size());
    for (const QUrl &url : urls) items.push_back(KFileItem(url, KFileItem::NormalMimeTypeDetermination));
    auto *actions = new KFileItemActions(submenu);
    actions->setParentWidget(m_parent);
    actions->setItemListProperties(KFileItemListProperties(items));
    actions->insertOpenWithActionsTo(nullptr, submenu, {});
    if (submenu->actions().isEmpty()) {
        QAction *none = submenu->addAction(trLocal("Brak pasujących aplikacji", "No matching applications"));
        none->setEnabled(false);
    }
}

bool SelectionMenuController::allUrlsAreLocalFiles(const QList<QUrl> &urls, bool allowDirectories) const
{
    if (urls.isEmpty()) return false;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile() || (!allowDirectories && QFileInfo(url.toLocalFile()).isDir())) return false;
    }
    return true;
}

bool SelectionMenuController::selectionHasCommonParent(const QList<QUrl> &urls, QString *parentPath) const
{
    if (!allUrlsAreLocalFiles(urls, true)) return false;
    QString common;
    for (const QUrl &url : urls) {
        const QString parent = QFileInfo(url.toLocalFile()).absolutePath();
        if (common.isEmpty()) common = parent;
        else if (QDir::cleanPath(common) != QDir::cleanPath(parent)) return false;
    }
    if (parentPath) *parentPath = common;
    return !common.isEmpty();
}

void SelectionMenuController::sendSelectionByEmail(const QList<QUrl> &urls) const
{
    if (!allUrlsAreLocalFiles(urls, false)) return;
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("xdg-email"));
    if (executable.isEmpty()) {
        QMessageBox::information(m_parent, trLocal("Wyślij e-mailem", "Send by e-mail"),
                                 trLocal("Nie znaleziono narzędzia xdg-email.", "xdg-email was not found."));
        return;
    }
    QStringList arguments;
    for (const QUrl &url : urls) arguments << QStringLiteral("--attach") << url.toLocalFile();
    arguments << QStringLiteral("--subject") << trLocal("Pliki z Ten komputer", "Files from This PC");
    if (!QProcess::startDetached(executable, arguments))
        QMessageBox::warning(m_parent, trLocal("Wyślij e-mailem", "Send by e-mail"),
                             trLocal("Nie udało się uruchomić domyślnego programu pocztowego.",
                                     "Could not start the default e-mail application."));
}

void SelectionMenuController::sendSelectionByBluetooth(const QList<QUrl> &urls) const
{
    if (!allUrlsAreLocalFiles(urls, false)) return;
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("bluedevil-sendfile"));
    if (executable.isEmpty()) {
        QMessageBox::information(m_parent, trLocal("Bluetooth", "Bluetooth"),
                                 trLocal("Nie znaleziono bluedevil-sendfile. Zainstaluj BlueDevil, aby wysyłać pliki przez Bluetooth.",
                                         "bluedevil-sendfile was not found. Install BlueDevil to send files over Bluetooth."));
        return;
    }
    QStringList arguments;
    for (const QUrl &url : urls) arguments << QStringLiteral("-f") << url.toLocalFile();
    if (!QProcess::startDetached(executable, arguments))
        QMessageBox::warning(m_parent, trLocal("Bluetooth", "Bluetooth"),
                             trLocal("Nie udało się uruchomić kreatora wysyłania Bluetooth.",
                                     "Could not start the Bluetooth file transfer wizard."));
}

void SelectionMenuController::addSendToSubmenu(QMenu &menu, const QList<QUrl> &urls,
                                                const SendToCallbacks &callbacks) const
{
    if (urls.isEmpty()) return;
    QMenu *send = menu.addMenu(themedIcon(QStringLiteral("document-send")), trLocal("Wyślij do", "Send to"));
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QAction *desktopAction = send->addAction(themedIcon(QStringLiteral("user-desktop")), trLocal("Pulpit", "Desktop"));
    desktopAction->setEnabled(!desktop.isEmpty());
    QObject::connect(desktopAction, &QAction::triggered, send, [=] {
        callbacks.copyToDirectory(urls, desktop, trLocal("Skopiowano na Pulpit", "Copied to Desktop"));
    });
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QAction *documentsAction = send->addAction(themedIcon(QStringLiteral("folder-documents")), trLocal("Dokumenty", "Documents"));
    documentsAction->setEnabled(!documents.isEmpty());
    QObject::connect(documentsAction, &QAction::triggered, send, [=] {
        callbacks.copyToDirectory(urls, documents, trLocal("Skopiowano do Dokumentów", "Copied to Documents"));
    });
    send->addSeparator();
    const bool localFiles = allUrlsAreLocalFiles(urls, false);
    QAction *bluetooth = send->addAction(themedIcon(QStringLiteral("preferences-system-bluetooth")), trLocal("Bluetooth…", "Bluetooth…"));
    bluetooth->setEnabled(localFiles && !QStandardPaths::findExecutable(QStringLiteral("bluedevil-sendfile")).isEmpty());
    QObject::connect(bluetooth, &QAction::triggered, send, [this, urls] { sendSelectionByBluetooth(urls); });
    QAction *email = send->addAction(themedIcon(QStringLiteral("mail-send")), trLocal("E-mail…", "E-mail…"));
    email->setEnabled(localFiles && !QStandardPaths::findExecutable(QStringLiteral("xdg-email")).isEmpty());
    QObject::connect(email, &QAction::triggered, send, [this, urls] { sendSelectionByEmail(urls); });
    send->addSeparator();
    const bool archives = selectionHasCommonParent(urls);
    QAction *zip = send->addAction(themedIcon(QStringLiteral("package-x-generic")), trLocal("Skompresowany plik ZIP…", "Compressed ZIP file…"));
    QAction *seven = send->addAction(themedIcon(QStringLiteral("package-x-generic")), trLocal("Skompresowany plik 7z…", "Compressed 7z file…"));
    QAction *tar = send->addAction(themedIcon(QStringLiteral("package-x-generic")), trLocal("Skompresowany plik tar.gz…", "Compressed tar.gz file…"));
    zip->setEnabled(archives); seven->setEnabled(archives); tar->setEnabled(archives);
    QObject::connect(zip, &QAction::triggered, send, [=] { callbacks.createZip(urls); });
    QObject::connect(seven, &QAction::triggered, send, [=] { callbacks.createSevenZip(urls); });
    QObject::connect(tar, &QAction::triggered, send, [=] { callbacks.createTarGzip(urls); });
}

void SelectionMenuController::addViewSubmenu(QMenu &menu, const ViewState &state,
                                              const ViewCallbacks &callbacks) const
{
    QMenu *view = menu.addMenu(themedIcon(QStringLiteral("view-list-icons")), trLocal("Widok", "View"));
    auto *group = new QActionGroup(view); group->setExclusive(true);
    struct Def { int mode; const char *pl; const char *en; const char *icon; };
    const Def defs[] = {{0,"Ikony","Icons","view-list-icons"},{1,"Lista","List","view-list-text"},
                        {2,"Szczegóły","Details","view-list-details"},{3,"Kompaktowy","Compact","view-list-tree"}};
    for (const Def &def : defs) {
        QAction *action = view->addAction(themedIcon(QString::fromLatin1(def.icon)), trLocal(def.pl, def.en));
        action->setCheckable(true); action->setChecked(state.viewMode == def.mode); group->addAction(action);
        QObject::connect(action, &QAction::triggered, view, [=] { callbacks.setViewMode(def.mode); });
    }
    view->addSeparator();
    QMenu *show = view->addMenu(themedIcon(QStringLiteral("view-visible"), QStringLiteral("view-preview")), trLocal("Pokaż", "Show"));
    QAction *hidden = show->addAction(themedIcon(QStringLiteral("view-hidden")), trLocal("Ukryte elementy", "Hidden items"));
    hidden->setCheckable(true); hidden->setChecked(state.showHidden);
    QObject::connect(hidden, &QAction::toggled, show, callbacks.setShowHidden);
    QAction *thumbs = show->addAction(themedIcon(QStringLiteral("view-preview")), trLocal("Pokaż podglądy", "Show Previews"));
    thumbs->setCheckable(true); thumbs->setChecked(state.thumbnails);
    QObject::connect(thumbs, &QAction::toggled, show, callbacks.setThumbnails);
    show->addAction(state.previewAction); show->addAction(state.fullNamesAction);
    addIconSizeStepControl(*view, state.iconSizeStep, state.iconSizeEnabled,
                           [=](int delta) {
                               callbacks.setIconSizeStep(state.iconSizeStep + delta);
                           });
}

void SelectionMenuController::addIconSizeStepControl(
    QMenu &menu, int rawStep, bool enabled,
    const std::function<void(int)> &setStep) const
{
    const int step = std::clamp(rawStep, 0, DirectoryViewSettings::iconSizeStepCount() - 1);
    QMenu *sizes = menu.addMenu(
        themedIcon(QStringLiteral("transform-scale"), QStringLiteral("view-list-icons")),
        trLocal("Rozmiar ikon", "Icon size"));
    sizes->setObjectName(QStringLiteral("viewIconSizeMenu"));
    sizes->setEnabled(enabled);
    QAction *smaller = sizes->addAction(trLocal("Mniejsze", "Smaller"));
    smaller->setObjectName(QStringLiteral("iconSizeSmaller"));
    smaller->setEnabled(step > 0);
    QAction *current = sizes->addAction(
        trLocal("Bieżący: %1 px", "Current: %1 px")
            .arg(DirectoryViewSettings::iconExtentForStep(step)));
    current->setObjectName(QStringLiteral("iconSizeCurrent"));
    current->setEnabled(false);
    QAction *larger = sizes->addAction(trLocal("Większe", "Larger"));
    larger->setObjectName(QStringLiteral("iconSizeLarger"));
    larger->setEnabled(step + 1 < DirectoryViewSettings::iconSizeStepCount());
    if (setStep) {
        QObject::connect(smaller, &QAction::triggered, sizes,
                         [=] { setStep(-1); });
        QObject::connect(larger, &QAction::triggered, sizes,
                         [=] { setStep(1); });
    }
}

void SelectionMenuController::addSortSubmenu(QMenu &menu, const ViewState &state,
                                              const ViewCallbacks &callbacks) const
{
    QMenu *sort = menu.addMenu(themedIcon(state.sortAscending ? QStringLiteral("view-sort-ascending") : QStringLiteral("view-sort-descending")), trLocal("Sortuj", "Sort"));
    auto *keys = new QActionGroup(sort); keys->setExclusive(true);
    const std::pair<int, QString> sorts[] = {{0,trLocal("Nazwa","Name")},{1,trLocal("Typ","Type")},{2,trLocal("Rozmiar","Size")},{3,trLocal("Data modyfikacji","Date modified")}};
    for (const auto &entry : sorts) {
        QAction *action = sort->addAction(entry.second); action->setCheckable(true); action->setChecked(state.sortKey == entry.first); keys->addAction(action);
        QObject::connect(action, &QAction::triggered, sort, [=] { callbacks.setSortKey(entry.first); });
    }
    sort->addSeparator();
    auto *directions = new QActionGroup(sort); directions->setExclusive(true);
    QAction *ascending = sort->addAction(themedIcon(QStringLiteral("view-sort-ascending")), trLocal("Rosnąco", "Ascending"));
    QAction *descending = sort->addAction(themedIcon(QStringLiteral("view-sort-descending")), trLocal("Malejąco", "Descending"));
    ascending->setCheckable(true); descending->setCheckable(true); ascending->setChecked(state.sortAscending); descending->setChecked(!state.sortAscending);
    directions->addAction(ascending); directions->addAction(descending);
    QObject::connect(ascending, &QAction::triggered, sort, [=] { callbacks.setSortAscending(true); });
    QObject::connect(descending, &QAction::triggered, sort, [=] { callbacks.setSortAscending(false); });
    sort->addSeparator();
    QMenu *groups = sort->addMenu(trLocal("Grupuj według", "Group by"));
    auto *groupActions = new QActionGroup(groups); groupActions->setExclusive(true);
    const std::pair<int, QString> modes[] = {{DirectoryViewSettings::NoGrouping,trLocal("Brak","None")},
        {DirectoryViewSettings::GroupByType,trLocal("Typ","Type")},{DirectoryViewSettings::GroupByDate,trLocal("Data modyfikacji","Date modified")},
        {DirectoryViewSettings::GroupBySize,trLocal("Rozmiar","Size")}};
    for (const auto &entry : modes) {
        QAction *action = groups->addAction(entry.second); action->setCheckable(true); action->setData(entry.first); action->setChecked(state.groupMode == entry.first); groupActions->addAction(action);
        QObject::connect(action, &QAction::triggered, groups, [=] { callbacks.setGroupMode(entry.first); });
    }
}

SelectionMenuController::PrintableKind SelectionMenuController::printableKindForUrl(const QUrl &url, bool isDir) const
{
    if (isDir || !url.isLocalFile()) return PrintableKind::None;
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(url.toLocalFile(), QMimeDatabase::MatchContent);
    if (!mime.isValid()) return PrintableKind::None;
    if (mime.name() == QStringLiteral("application/pdf"))
        return QStandardPaths::findExecutable(QStringLiteral("okular")).isEmpty() ? PrintableKind::None : PrintableKind::Pdf;
    if (mime.name().startsWith(QStringLiteral("image/"))) return PrintableKind::Image;
    if (mime.name().startsWith(QStringLiteral("text/"))) return PrintableKind::Text;
    return PrintableKind::None;
}

bool SelectionMenuController::canPrintUrl(const QUrl &url, bool isDir) const { return printableKindForUrl(url, isDir) != PrintableKind::None; }
bool SelectionMenuController::isLocalImageUrl(const QUrl &url, bool isDir) const
{
    if (isDir || !url.isLocalFile()) return false;
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(url.toLocalFile(), QMimeDatabase::MatchContent);
    return mime.isValid() && mime.name().startsWith(QStringLiteral("image/"));
}
bool SelectionMenuController::canSetWallpaper(const QUrl &url, bool isDir) const
{
    return isLocalImageUrl(url, isDir) && !QStandardPaths::findExecutable(QStringLiteral("plasma-apply-wallpaperimage")).isEmpty();
}

void SelectionMenuController::printImageUrl(const QUrl &url) const
{
    QImageReader reader(url.toLocalFile()); reader.setAutoTransform(true); const QImage image = reader.read();
    if (image.isNull()) {
        QMessageBox::warning(m_parent, trLocal("Drukowanie","Printing"), isPolish()
            ? QStringLiteral("Nie udało się odczytać obrazu:\n%1").arg(reader.errorString())
            : QStringLiteral("Could not read the image:\n%1").arg(reader.errorString()));
        return;
    }
    QPrinter printer(QPrinter::HighResolution); printer.setDocName(QFileInfo(url.toLocalFile()).fileName());
    QPrintDialog dialog(&printer, m_parent); dialog.setWindowTitle(trLocal("Drukuj obraz","Print image")); if (dialog.exec() != QDialog::Accepted) return;
    QPainter painter(&printer);
    if (!painter.isActive()) {
        QMessageBox::warning(m_parent, trLocal("Drukowanie","Printing"),
            trLocal("Nie udało się rozpocząć drukowania.", "Could not start printing."));
        return;
    }
    const QRect page = printer.pageLayout().paintRectPixels(printer.resolution()); QSize size = image.size(); size.scale(page.size(), Qt::KeepAspectRatio);
    painter.drawImage(QRect(page.x() + (page.width()-size.width())/2, page.y() + (page.height()-size.height())/2, size.width(), size.height()), image);
}

void SelectionMenuController::printTextUrl(const QUrl &url) const
{
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly|QIODevice::Text)) {
        QMessageBox::warning(m_parent, trLocal("Drukowanie","Printing"),
            trLocal("Nie udało się otworzyć pliku tekstowego.", "Could not open the text file."));
        return;
    }
    if (file.size() > 32LL*1024LL*1024LL) {
        QMessageBox::warning(m_parent, trLocal("Drukowanie","Printing"),
            trLocal("Plik tekstowy jest zbyt duży do bezpośredniego drukowania (limit 32 MiB).",
                    "The text file is too large for direct printing (32 MiB limit)."));
        return;
    }
    QTextDocument document; document.setPlainText(QString::fromUtf8(file.readAll())); QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(QFileInfo(url.toLocalFile()).fileName()); QPrintDialog dialog(&printer,m_parent); dialog.setWindowTitle(trLocal("Drukuj dokument tekstowy","Print text document"));
    if (dialog.exec()==QDialog::Accepted) document.print(&printer);
}

void SelectionMenuController::printUrl(const QUrl &url) const
{
    switch (printableKindForUrl(url,false)) {
    case PrintableKind::Image: printImageUrl(url); return;
    case PrintableKind::Text: printTextUrl(url); return;
    case PrintableKind::Pdf: {
        const QString okular=QStandardPaths::findExecutable(QStringLiteral("okular"));
        if (okular.isEmpty() || !QProcess::startDetached(okular,{QStringLiteral("--print"),url.toLocalFile()}))
            QMessageBox::warning(m_parent,trLocal("Drukowanie","Printing"),trLocal("Nie udało się uruchomić okna drukowania PDF w Okularze.","Could not start the PDF print dialog in Okular."));
        return; }
    default: QMessageBox::information(m_parent,trLocal("Drukowanie","Printing"),trLocal("Ten typ pliku nie ma jeszcze obsługi drukowania.","This file type does not have printing support yet."));
    }
}

void SelectionMenuController::setAsDesktopWallpaper(const QUrl &url, const std::function<void(const QString &)> &status) const
{
    if (!isLocalImageUrl(url,false)) return;
    const QString executable=QStandardPaths::findExecutable(QStringLiteral("plasma-apply-wallpaperimage"));
    if (executable.isEmpty()) { QMessageBox::information(m_parent,trLocal("Tło pulpitu","Desktop wallpaper"),trLocal("Nie znaleziono narzędzia plasma-apply-wallpaperimage.","plasma-apply-wallpaperimage was not found.")); return; }
    if (!QProcess::startDetached(executable,{url.toLocalFile()})) { QMessageBox::warning(m_parent,trLocal("Tło pulpitu","Desktop wallpaper"),trLocal("Nie udało się ustawić obrazu jako tła pulpitu.","Could not set the image as the desktop wallpaper.")); return; }
    status(trLocal("Przekazano obraz do ustawienia jako tło pulpitu.","The image was sent to Plasma as the desktop wallpaper."));
}
