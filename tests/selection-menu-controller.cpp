#include "selectionmenucontroller.h"

#include <QAction>
#include <QFile>
#include <QMenu>
#include <QTemporaryDir>

static int checks = 0;
static void verify(bool condition, const char *message)
{
    ++checks;
    if (!condition) qFatal("selection menu: %s", message);
}

static QMenu *submenu(QMenu &menu, const QString &text)
{
    for (QAction *action : menu.actions())
        if (action->text() == text) return action->menu();
    return nullptr;
}

static QAction *action(QMenu &menu, const QString &text)
{
    for (QAction *candidate : menu.actions()) {
        if (candidate->text() == text) return candidate;
        if (candidate->menu())
            if (QAction *nested = action(*candidate->menu(), text)) return nested;
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QWidget parent;
    SelectionMenuController controller(&parent);
    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary directory");
    const QString filePath = temporary.filePath(QStringLiteral("note.txt"));
    QFile file(filePath);
    verify(file.open(QIODevice::WriteOnly) && file.write("text") == 4, "text fixture");
    file.close();
    const QString folderPath = temporary.filePath(QStringLiteral("folder"));
    verify(QDir().mkpath(folderPath), "folder fixture");
    const QUrl localFile = QUrl::fromLocalFile(filePath);
    const QUrl localFolder = QUrl::fromLocalFile(folderPath);
    const QUrl remote(QStringLiteral("sftp://example.test/note.txt"));

    verify(controller.allUrlsAreLocalFiles({localFile}, false), "single local file eligibility");
    verify(!controller.allUrlsAreLocalFiles({}, false), "empty selection eligibility");
    verify(!controller.allUrlsAreLocalFiles({localFolder}, false), "folder is not a plain local file");
    verify(controller.allUrlsAreLocalFiles({localFolder}, true), "folder allowed for archives");
    verify(!controller.allUrlsAreLocalFiles({remote}, true), "KIO URL is not local");
    verify(controller.selectionHasCommonParent({localFile, localFolder}), "multi selection common parent");
    verify(!controller.selectionHasCommonParent({localFile, remote}), "mixed local and KIO archive selection rejected");
    verify(controller.canPrintUrl(localFile, false), "local text is printable");
    verify(!controller.canPrintUrl(remote, false), "KIO URL is not printable");
    verify(!controller.canPrintUrl(localFolder, true), "folder is not printable");

    QMenu empty(&parent);
    controller.addOpenWithSubmenu(empty, {});
    controller.addSendToSubmenu(empty, {}, {});
    verify(empty.actions().isEmpty(), "no-selection menu adds no selection submenus");

    QMenu single(&parent);
    controller.addOpenWithSubmenu(single, {localFile});
    int archiveRoute = 0;
    controller.addSendToSubmenu(single, {localFile}, {
        [](const QList<QUrl> &, const QString &, const QString &) {},
        [&](const QList<QUrl> &) { archiveRoute = 1; },
        [&](const QList<QUrl> &) { archiveRoute = 2; },
        [&](const QList<QUrl> &) { archiveRoute = 3; }});
    verify(submenu(single, trLocal("Otwórz za pomocą", "Open with")), "Open With present for file");
    QMenu *send = submenu(single, trLocal("Wyślij do", "Send to"));
    verify(send, "Send To present for file");
    QAction *zip = action(*send, trLocal("Skompresowany plik ZIP…", "Compressed ZIP file…"));
    verify(zip && zip->isEnabled(), "archive creation enabled for local selection");
    zip->trigger();
    verify(archiveRoute == 1, "ZIP routes through supplied backend callback");

    QMenu remoteMenu(&parent);
    controller.addSendToSubmenu(remoteMenu, {remote}, {});
    QMenu *remoteSend = submenu(remoteMenu, trLocal("Wyślij do", "Send to"));
    verify(remoteSend, "Send To remains present for KIO selection");
    verify(!action(*remoteSend, trLocal("Skompresowany plik ZIP…", "Compressed ZIP file…"))->isEnabled(),
           "archive creation disabled for KIO selection");
    verify(!action(*remoteSend, trLocal("E-mail…", "E-mail…"))->isEnabled(),
           "email disabled for KIO selection");
    verify(!action(*remoteSend, trLocal("Bluetooth…", "Bluetooth…"))->isEnabled(),
           "Bluetooth disabled for KIO selection");

    int viewMode = -1, iconSize = -1, sortKey = -1, groupMode = -1;
    bool ascending = false, hidden = false, thumbnails = true;
    QAction preview(QStringLiteral("Preview"), &parent), fullNames(QStringLiteral("Full names"), &parent);
    SelectionMenuController::ViewState state{PaneId::Split, 2, 1, 3, false,
        DirectoryViewSettings::GroupByDate, false, true, &preview, &fullNames};
    SelectionMenuController::ViewCallbacks callbacks{
        [&](int value) { viewMode = value; }, [&](int value) { iconSize = value; },
        [&](int value) { sortKey = value; }, [&](bool value) { ascending = value; },
        [&](int value) { groupMode = value; }, [&](bool value) { hidden = value; },
        [&](bool value) { thumbnails = value; }};
    QMenu background(&parent);
    controller.addViewSubmenu(background, state, callbacks);
    controller.addSortSubmenu(background, state, callbacks);
    verify(submenu(background, trLocal("Widok", "View")), "View submenu present");
    verify(submenu(background, trLocal("Sortuj", "Sort")), "Sort submenu present");
    action(background, trLocal("Ikony", "Icons"))->trigger();
    action(background, trLocal("Małe", "Small"))->trigger();
    QMenu *sortMenu = submenu(background, trLocal("Sortuj", "Sort"));
    action(*sortMenu, trLocal("Nazwa", "Name"))->trigger();
    action(*sortMenu, trLocal("Rosnąco", "Ascending"))->trigger();
    QMenu *groupMenu = submenu(*sortMenu, trLocal("Grupuj według", "Group by"));
    action(*groupMenu, trLocal("Rozmiar", "Size"))->trigger();
    action(background, trLocal("Ukryte elementy", "Hidden items"))->trigger();
    action(background, trLocal("Miniatury obrazów", "Image thumbnails"))->trigger();
    verify(viewMode == 0 && iconSize == 3, "View routes to active-pane callbacks");
    verify(sortKey == 0 && ascending, "Sort routes to active-pane callbacks");
    verify(groupMode == DirectoryViewSettings::GroupBySize, "Grouping routes to active-pane callback");
    verify(hidden && !thumbnails, "Show toggles route through callbacks");

    qInfo("PASS: %d selection menu controller assertions", checks);
    return 0;
}
