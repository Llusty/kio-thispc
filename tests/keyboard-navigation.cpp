#include "keyboardnavigation.h"
#include "directoryview.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QGridLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QTest>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QWidget scope;
    auto *layout = new QGridLayout(&scope);
    QWidget a, b, c, d, e;
    for (QWidget *card : {&a, &b, &c, &d, &e}) card->setFocusPolicy(Qt::StrongFocus);
    a.setProperty("navigationUrl", "test:/a"); b.setProperty("navigationUrl", "test:/b");
    c.setProperty("navigationUrl", "test:/c"); d.setProperty("navigationUrl", "test:/d");
    e.setProperty("navigationUrl", "test:/e");
    layout->addWidget(&a, 0, 0); layout->addWidget(&b, 0, 1); layout->addWidget(&c, 0, 2);
    layout->addWidget(&d, 1, 0); layout->addWidget(&e, 1, 1);
    scope.resize(600, 240); scope.show(); app.processEvents();

    int activated = 0, back = 0, forward = 0, up = 0, iconSizeDelta = 0;
    KeyboardNavigationRouter::Context context = KeyboardNavigationRouter::Context::FileView;
    QString currentHomeId = QStringLiteral("test:/a");
    QList<QWidget *> activeCards{&a, &b, &c, &d, &e};
    KeyboardNavigationRouter::Callbacks callbacks;
    callbacks.contextForFocus = [&](QWidget *focus) {
            return focus && (focus == &scope || scope.isAncestorOf(focus))
                ? context : KeyboardNavigationRouter::Context::None;
        };
    callbacks.activateCurrent = [&] { ++activated; };
    callbacks.back = [&] { ++back; };
    callbacks.forward = [&] { ++forward; };
    callbacks.up = [&] { ++up; };
    callbacks.adjustIconSizeStep = [&](int delta) { iconSizeDelta += delta; };
    callbacks.homeCards = [&] { return activeCards; };
    callbacks.currentHomeCard = [&] {
        for (QWidget *card : activeCards) {
            if (card->property("navigationUrl").toString() == currentHomeId) return card;
        }
        return static_cast<QWidget *>(nullptr);
    };
    callbacks.setCurrentHomeCard = [&](QWidget *card) {
        currentHomeId = card->property("navigationUrl").toString();
    };
    KeyboardNavigationRouter router(std::move(callbacks));
    qApp->installEventFilter(&router);

    a.setFocus(); QTest::keyClick(&a, Qt::Key_Return);
    verify(activated == 1, "Enter activates the current file-view item");
    QTest::keyClick(&a, Qt::Key_Backspace);
    verify(up == 1, "Backspace routes to Up in a file view");
    QTest::keyClick(&a, Qt::Key_Left, Qt::AltModifier);
    QTest::keyClick(&a, Qt::Key_Right, Qt::AltModifier);
    QTest::keyClick(&a, Qt::Key_Up, Qt::AltModifier);
    verify(back == 1 && forward == 1 && up == 2,
           "Alt Left Right Up route through pane callbacks");
    QTest::keyClick(&a, Qt::Key_Plus, Qt::ControlModifier);
    QTest::keyClick(&a, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyClick(&a, Qt::Key_Equal, Qt::ControlModifier);
    QTest::keyClick(&a, Qt::Key_Minus, Qt::ControlModifier);
    verify(iconSizeDelta == 2,
           "Ctrl Plus, shifted Ctrl Plus, Ctrl Equal, and Ctrl Minus route icon-size steps");
    QTest::keyClick(&a, Qt::Key_Plus);
    QTest::keyClick(&a, Qt::Key_Equal);
    QTest::keyClick(&a, Qt::Key_Minus);
    verify(iconSizeDelta == 2, "plain Plus, Equal, and Minus are not intercepted");

    QLineEdit line(&scope); line.show(); line.setText("abc"); line.setFocus();
    QTest::keyClick(&line, Qt::Key_Backspace);
    verify(line.text() == "ab" && up == 2, "QLineEdit owns Backspace");
    QTest::keyClick(&line, Qt::Key_Plus, Qt::ControlModifier);
    verify(iconSizeDelta == 2, "QLineEdit owns Ctrl Plus");
    directory_view_detail::IconNameEditor rename(&scope);
    rename.setFileName("rename.txt"); rename.show(); rename.setFocus();
    QTest::keyClick(&rename, Qt::Key_Backspace);
    verify(up == 2, "IconNameEditor owns Backspace");
    QTest::keyClick(&rename, Qt::Key_Minus, Qt::ControlModifier);
    verify(iconSizeDelta == 2, "IconNameEditor owns Ctrl Minus");
    QComboBox combo(&scope); combo.setEditable(true); combo.show(); combo.setFocus();
    QTest::keyClick(&combo, Qt::Key_Left, Qt::AltModifier);
    verify(back == 1, "editable combo owns Alt navigation");

    context = KeyboardNavigationRouter::Context::Home;
    a.setFocus(); QTest::keyClick(&a, Qt::Key_Right);
    verify(b.hasFocus(), "home Right follows the visual row");
    QTest::keyClick(&b, Qt::Key_Right);
    verify(c.hasFocus(), "home Right advances to the next card");
    QTest::keyClick(&c, Qt::Key_Right);
    verify(c.hasFocus(), "home right boundary is a no-op");
    QTest::keyClick(&c, Qt::Key_Down);
    verify(e.hasFocus(), "home Down chooses the nearest card in the next row");
    QTest::keyClick(&e, Qt::Key_Left);
    verify(d.hasFocus(), "home Left follows the second visual row");
    QTest::keyClick(&d, Qt::Key_Up);
    verify(a.hasFocus(), "home Up preserves the intuitive column");
    QTest::keyClick(&a, Qt::Key_Up);
    verify(a.hasFocus(), "home upper boundary is a no-op");
    QTest::keyClick(&a, Qt::Key_Left);
    verify(a.hasFocus() && currentHomeId == QStringLiteral("test:/a"),
           "home left boundary preserves logical current");
    currentHomeId = QStringLiteral("test:/c"); c.setFocus();
    QTest::keyClick(&c, Qt::Key_Right);
    verify(c.hasFocus() && currentHomeId == QStringLiteral("test:/c"),
           "home right boundary preserves logical current");
    currentHomeId = QStringLiteral("test:/e"); e.setFocus();
    QTest::keyClick(&e, Qt::Key_Down);
    verify(e.hasFocus() && currentHomeId == QStringLiteral("test:/e"),
           "home lower boundary preserves logical current");

    const int alternatingKeys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                                   Qt::Key_Down, Qt::Key_Right, Qt::Key_Down};
    for (int i = 0; i < 60; ++i) {
        QWidget *logicalCurrent = nullptr;
        for (QWidget *card : activeCards) {
            if (card->property("navigationUrl").toString() == currentHomeId)
                logicalCurrent = card;
        }
        verify(logicalCurrent != nullptr, "repeated arrows never lose logical current");
        QTest::keyClick(logicalCurrent, Qt::Key(alternatingKeys[i % 6]));
    }

    QList<QWidget *> rebuilt{&a, &b, &c};
    verify(KeyboardNavigationRouter::homeGridTarget(&e, Qt::Key_Down, rebuilt) == &a,
           "a rebuilt grid recovers from a stale focus identity without dereferencing it");
    activeCards.clear();
    currentHomeId.clear();
    verify(KeyboardNavigationRouter::homeGridTarget(nullptr, Qt::Key_Down, activeCards) == nullptr,
           "empty home grid is a safe no-op");
    activeCards = {&a, &b, &c, &d, &e};
    currentHomeId = QStringLiteral("test:/a");

    QDialog modal(&scope); modal.setWindowModality(Qt::ApplicationModal); modal.show();
    app.processEvents(); a.setFocus(); QTest::keyClick(&a, Qt::Key_Backspace);
    verify(up == 2, "active modal dialog blocks routing");
    QTest::keyClick(&a, Qt::Key_Plus, Qt::ControlModifier);
    verify(iconSizeDelta == 2, "active modal dialog blocks icon-size shortcuts");
    modal.hide(); app.processEvents();

    QMenu popup(&scope); popup.addAction("item"); popup.popup(scope.mapToGlobal(QPoint(10, 10)));
    app.processEvents(); QTest::keyClick(&popup, Qt::Key_Left, Qt::AltModifier);
    verify(back == 1, "active popup blocks routing");
    QTest::keyClick(&popup, Qt::Key_Minus, Qt::ControlModifier);
    verify(iconSizeDelta == 2, "active popup and menu block icon-size shortcuts");
    popup.close();

    context = KeyboardNavigationRouter::Context::None;
    a.setFocus(); QTest::keyClick(&a, Qt::Key_Backspace);
    verify(up == 2, "unowned focus is not intercepted");
    verify(KeyboardNavigationRouter::isTextInput(&line)
               && KeyboardNavigationRouter::isTextInput(rename.viewport()),
           "text-input guard recognizes direct editors and child widgets");

    QTemporaryDir files;
    verify(files.isValid() && QDir().mkpath(files.filePath("parent/child")),
           "integration directory fixture");
    const QUrl parent = QUrl::fromLocalFile(files.filePath("parent"));
    const QUrl child = QUrl::fromLocalFile(files.filePath("parent/child"));
    ThisPcWindow window(parent, false);
    window.show(); window.activateWindow();
    auto waitForListing = [&] {
        return QTest::qWaitFor([&] { return window.m_primaryPane->listingJob() == nullptr; }, 3000);
    };
    verify(waitForListing(), "initial listing completes");

    for (int mode : {0, 1, 3}) {
        window.m_directoryViewMode = mode;
        window.applyDirectoryViewMode(false);
        QModelIndex target;
        for (int row = 0; row < window.m_directoryList->model()->rowCount(); ++row) {
            const QModelIndex candidate = window.m_directoryList->model()->index(row, 0);
            if (QUrl(candidate.data(directory_view_detail::UrlRole).toString()) == child) {
                target = candidate; break;
            }
        }
        verify(target.isValid(), "list mode exposes the child directory");
        window.m_directoryList->selectionModel()->setCurrentIndex(
            target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Current);
        window.m_directoryList->setFocus();
        QTest::keyClick(window.m_directoryList, Qt::Key_Return);
        verify(QTest::qWaitFor([&] { return sameLocation(window.m_navigation.currentUrl(), child); }, 1000),
               mode == 0 ? "Icons Enter activates current item"
                         : mode == 1 ? "List Enter activates current item"
                                     : "Compact Enter activates current item");
        window.navigateTo(parent, true);
        verify(waitForListing(), "listing returns to the parent between view modes");
    }

    window.m_directoryViewMode = 2;
    window.applyDirectoryViewMode(false);
    QTreeWidgetItem *detailsTarget = nullptr;
    for (int row = 0; row < window.m_directoryDetails->topLevelItemCount(); ++row) {
        QTreeWidgetItem *candidate = window.m_directoryDetails->topLevelItem(row);
        if (QUrl(candidate->data(0, Qt::UserRole).toString()) == child) {
            detailsTarget = candidate; break;
        }
    }
    verify(detailsTarget != nullptr, "Details mode exposes the child directory");
    window.m_directoryDetails->setCurrentItem(detailsTarget);
    window.m_directoryDetails->setFocus();
    QTest::keyClick(window.m_directoryDetails, Qt::Key_Return);
    verify(QTest::qWaitFor([&] { return sameLocation(window.m_navigation.currentUrl(), child); }, 1000),
           "Details Enter activates current item");
    verify(waitForListing(), "child listing completes before Backspace");
    QWidget *activeView = window.paneContext().view;
    activeView->setFocus();
    QTest::keyClick(activeView, Qt::Key_Backspace);
    verify(QTest::qWaitFor([&] { return sameLocation(window.m_navigation.currentUrl(), parent); }, 1000),
           "Backspace navigates the active real pane to its parent");
    verify(waitForListing(), "parent listing completes after Backspace");

    auto drive = [](const QString &name, const QUrl &url) {
        DriveInfo info;
        info.name = name;
        info.targetUrl = url;
        info.capacityText = QStringLiteral("100 GiB");
        info.freeText = QStringLiteral("50 GiB");
        return info;
    };
    const QUrl driveA = QUrl::fromLocalFile(files.filePath("drive-a"));
    const QUrl driveB = QUrl::fromLocalFile(files.filePath("drive-b"));
    QDir().mkpath(driveA.toLocalFile()); QDir().mkpath(driveB.toLocalFile());
    window.navigateTo(kThisPcUrl, true);
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    auto cardFor = [&](PaneId pane, const QUrl &url) {
        const QString id = url.toString(QUrl::FullyEncoded);
        for (QWidget *card : window.homeCards(pane)) {
            if (card->property("navigationUrl").toString() == id) return card;
        }
        return static_cast<QWidget *>(nullptr);
    };
    QWidget *primaryB = cardFor(PaneId::Primary, driveB);
    verify(primaryB != nullptr, "primary home exposes drive B");
    QTest::mouseClick(primaryB, Qt::LeftButton); app.processEvents();
    verify(window.m_primaryHomeCurrentId == driveB.toString(QUrl::FullyEncoded),
           "mouse focus updates stable primary current URL");

    auto *primaryBackground = window.m_homePage->findChild<HomePageWidget *>();
    verify(primaryBackground != nullptr, "primary home exposes an explicit background target");
    QTest::mouseClick(primaryBackground, Qt::LeftButton, Qt::NoModifier,
                      primaryBackground->rect().bottomRight() - QPoint(2, 2));
    app.processEvents();
    verify(window.m_primaryHomeCurrentId.isEmpty() && !primaryB->hasFocus(),
           "primary background click clears logical and visual current");
    for (int refresh = 0; refresh < 3; ++refresh) {
        const QList<DriveInfo> snapshot = refresh == 1
            ? QList<DriveInfo>{drive(QStringLiteral("B"), driveB),
                               drive(QStringLiteral("A"), driveA)}
            : QList<DriveInfo>{drive(QStringLiteral("A"), driveA),
                               drive(QStringLiteral("B"), driveB)};
        window.m_driveHomeCoordinator.setSnapshotForTesting(snapshot);
        app.processEvents();
        verify(window.m_primaryHomeCurrentId.isEmpty()
                   && window.currentHomeCard(PaneId::Primary) == nullptr,
               "rebuild and reorder preserve explicit primary no-current");
    }
    QTest::keyClick(primaryBackground, Qt::Key_Down);
    app.processEvents();
    verify(window.currentHomeCard(PaneId::Primary) != nullptr,
           "an arrow from no-current starts at the first visible card");
    window.clearCurrentHomeCard(PaneId::Primary);
    const QUrl beforeNoCurrentEnter = window.m_navigation.currentUrl();
    QTest::keyClick(primaryBackground, Qt::Key_Return);
    app.processEvents();
    verify(sameLocation(window.m_navigation.currentUrl(), beforeNoCurrentEnter),
           "Enter from no-current is a no-op");
    primaryB = cardFor(PaneId::Primary, driveB);
    primaryB->setFocus(Qt::MouseFocusReason); app.processEvents();
    window.m_addressEdit->setFocus(); app.processEvents();
    verify(window.m_primaryHomeCurrentId == driveB.toString(QUrl::FullyEncoded),
           "focus loss to the address bar preserves logical current");
    primaryB->setFocus(Qt::MouseFocusReason); app.processEvents();

    QPointer<QWidget> stalePrimaryB = primaryB;
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    app.processEvents();
    QWidget *restoredPrimaryB = cardFor(PaneId::Primary, driveB);
    verify(restoredPrimaryB && restoredPrimaryB->hasFocus()
               && window.m_primaryHomeCurrentId == driveB.toString(QUrl::FullyEncoded),
           "same-dataset rebuild restores URL and focus");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    app.processEvents();
    verify(stalePrimaryB.isNull() && window.currentHomeCard(PaneId::Primary) == restoredPrimaryB,
           "deferred deletion cannot leave current on a stale widget");

    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("B"), driveB), drive(QStringLiteral("A"), driveA)});
    app.processEvents();
    verify(cardFor(PaneId::Primary, driveB)->hasFocus()
               && window.m_primaryHomeCurrentId == driveB.toString(QUrl::FullyEncoded),
           "reordered rebuild preserves current by stable URL");

    window.m_driveHomeCoordinator.setSnapshotForTesting({drive(QStringLiteral("A"), driveA)});
    app.processEvents();
    QWidget *fallback = window.currentHomeCard(PaneId::Primary);
    verify(fallback && fallback->hasFocus()
               && fallback->property("navigationUrl").toString()
                    == driveA.toString(QUrl::FullyEncoded),
           "removed current drive falls back to the nearest previous position");
    QTest::keyClick(fallback, Qt::Key_Left);
    verify(window.currentHomeCard(PaneId::Primary) != nullptr,
           "arrows continue immediately after a drive refresh callback");

    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    QWidget *enterCard = cardFor(PaneId::Primary, driveB);
    enterCard->setFocus(Qt::MouseFocusReason); app.processEvents();
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    QTest::keyClick(cardFor(PaneId::Primary, driveB), Qt::Key_Return);
    verify(QTest::qWaitFor([&] { return sameLocation(window.m_navigation.currentUrl(), driveB); }, 1000),
           "Enter after refresh activates the restored card URL");

    window.navigateTo(kThisPcUrl, true);
    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(kThisPcUrl);
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    QWidget *splitB = cardFor(PaneId::Split, driveB);
    verify(splitB != nullptr, "split home exposes the same drive cards");
    splitB->setFocus(Qt::MouseFocusReason); app.processEvents();
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("B"), driveB), drive(QStringLiteral("A"), driveA)});
    app.processEvents();
    verify(cardFor(PaneId::Split, driveB)->hasFocus()
               && window.m_splitHomeCurrentId == driveB.toString(QUrl::FullyEncoded),
           "split home rebuild has primary parity for stable current restore");

    auto *splitBackground = window.m_splitHomePage->findChild<HomePageWidget *>();
    verify(splitBackground != nullptr, "split home exposes an explicit background target");
    QTest::mouseClick(splitBackground, Qt::LeftButton, Qt::NoModifier,
                      splitBackground->rect().bottomRight() - QPoint(2, 2));
    app.processEvents();
    window.m_driveHomeCoordinator.setSnapshotForTesting(
        {drive(QStringLiteral("A"), driveA), drive(QStringLiteral("B"), driveB)});
    app.processEvents();
    verify(window.m_splitHomeCurrentId.isEmpty()
               && window.currentHomeCard(PaneId::Split) == nullptr,
           "split background clear survives rebuild with primary parity");

    window.setSplitViewEnabled(false);
    window.navigateTo(parent, true);
    verify(waitForListing(), "primary listing restored after home-grid integration cases");

    window.m_directoryDetails->setCurrentItem(nullptr);
    window.m_directoryDetails->clearSelection();
    const QUrl beforeNoCurrent = window.m_navigation.currentUrl();
    window.m_directoryDetails->setFocus();
    QTest::keyClick(window.m_directoryDetails, Qt::Key_Return);
    verify(sameLocation(window.m_navigation.currentUrl(), beforeNoCurrent),
           "Enter with no current or selected item is a no-op");

    window.m_searchEdit->setText("abc"); window.m_searchEdit->setFocus();
    QTest::keyClick(window.m_searchEdit, Qt::Key_Backspace);
    verify(window.m_searchEdit->text() == "ab"
               && sameLocation(window.m_navigation.currentUrl(), parent),
           "Search owns Backspace without pane navigation");

    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(child);
    window.setActivePane(PaneId::Split);
    window.m_splitPane->listView()->setFocus();
    QTest::keyClick(window.m_splitPane->listView(), Qt::Key_Backspace);
    verify(QTest::qWaitFor([&] { return sameLocation(window.m_splitPane->currentUrl(), parent); }, 1000)
               && sameLocation(window.m_navigation.currentUrl(), parent),
           "Backspace routes only through the active Split pane");

    qInfo("PASS: %d keyboard navigation assertions", checks);
}
