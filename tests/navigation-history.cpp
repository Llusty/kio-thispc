/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "navigationhistory.h"
#include "browsercommon.h"
#include "searchcontroller.h"
#include <QCoreApplication>
static int checks = 0;
static void verify(bool value, const char *description) { if (!value) qFatal("FAIL: %s", description); ++checks; }
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    NavigationHistory n;
    verify(n.currentUrl() == kThisPcUrl && n.history().isEmpty() && n.historyIndex() == -1
               && !n.canGoBack() && !n.canGoForward() && !n.canGoUp(), "initial state");
    const QUrl a = QUrl::fromLocalFile("/tmp/A"), b = QUrl::fromLocalFile("/tmp/A/B"), c = QUrl::fromLocalFile("/tmp/A/B/C");
    n.navigate(a); n.navigate(b); n.navigate(c);
    verify(n.currentUrl() == c && n.history() == QList<QUrl>({a,b,c}) && n.historyIndex() == 2
               && n.canGoBack() && !n.canGoForward(), "navigate A-B-C");
    verify(n.back() == b && n.back() == a && n.forward() == b && n.canGoBack() && n.canGoForward(), "back back forward");
    const QUrl d = QUrl::fromLocalFile("/tmp/D"); n.navigate(d);
    verify(n.history() == QList<QUrl>({a,b,d}) && !n.canGoForward(), "forward truncation");
    const int size = n.history().size();
    verify(n.navigate(d) && n.history().size() == size && n.currentUrl() == d, "duplicate reload semantics");
    QVector<DriveInfo> drives;
    verify(n.parentUrl(drives) == QUrl::fromLocalFile("/tmp"), "local parent");
    n.navigate(QUrl::fromLocalFile("/"));
    verify(n.parentUrl(drives) == kThisPcUrl, "root parent");
    n.navigate(kThisPcUrl);
    verify(!n.parentUrl(drives).isValid() && !n.canGoUp(), "This PC edge");
    const QUrl search = makeSearchLocation("needle", 0, b, 0, 0, 0); n.navigate(search);
    verify(n.parentUrl(drives) == b, "Search parent");
    n.navigate(QUrl("admin:/etc/systemd"));
    verify(n.parentUrl(drives) == QUrl("admin:/etc"), "admin parent");
    n.navigate(QUrl("sftp://host/share/folder"));
    verify(n.parentUrl(drives) == QUrl("sftp://host/share"), "remote KIO parent");
    NavigationHistory restored; restored.restore(n.snapshot());
    verify(restored.currentUrl() == n.currentUrl() && restored.history() == n.history()
               && restored.historyIndex() == n.historyIndex() && restored.canGoBack() == n.canGoBack()
               && restored.canGoForward() == n.canGoForward(), "snapshot restore");
    restored.restore({b, {}, -1});
    verify(restored.history() == QList<QUrl>({b}) && restored.historyIndex() == 0, "history reconstruction");
    restored.restore({c, {a,b}, 99});
    verify(restored.historyIndex() == 1 && restored.currentUrl() == c, "restore clamping");
    qInfo("PASS: %d NavigationHistory assertions", checks);
}
