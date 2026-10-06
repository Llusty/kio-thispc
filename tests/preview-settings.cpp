/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

// Appended to the temporary regression binary by run-pane-actions.py.
// 0.39 Stage 6: Show Previews toggle behavior.

#include <QApplication>
#include <QFile>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include "directorypreviewadapter.h"
#include "directoryview.h"
#include "previewcontroller.h"
#include "primarybrowserpane.h"
#include "splitbrowserpane.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int checks = 0;
    auto verify = [&](bool condition, const char *message) {
        ++checks;
        if (!condition) qFatal("FAIL: %s", message);
    };

    QTemporaryDir tempDir;
    verify(tempDir.isValid(), "temp dir");
    const QUrl base = QUrl::fromLocalFile(tempDir.path());
    const QUrl png = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("a.png")));

    // Plugin policy: neutral KDE defaults, no application-specific rc dependency.
    const QStringList plugins = PreviewController::queryDefaultPlugins();
    verify(!plugins.isEmpty(), "1. default plugins not empty");
    verify(plugins.contains(QStringLiteral("imagethumbnail")), "2. image thumbnailer present");
    verify(plugins.contains(QStringLiteral("directorythumbnail")), "3. directory thumbnailer present");
    verify(plugins.contains(QStringLiteral("windowsexethumbnail")), "4. exe thumbnailer present");

    DirectoryListWidget list;
    list.resize(400, 300);
    DirectoryTreeWidget tree;
    tree.resize(400, 300);
    FileInfo fi;
    fi.name = QStringLiteral("a.png");
    fi.url = png;
    fi.isDir = false;
    fi.mimeType = QStringLiteral("image/png");
    addDirectoryFileItems(&list, &tree, fi, list.style()->standardIcon(QStyle::SP_FileIcon),
                          QStringLiteral("PNG"), QStringLiteral("1 KB"), QStringLiteral("Today"));
    const QModelIndex idx = list.model()->index(0, 0);
    verify(idx.isValid(), "5. item exists");

    DirectoryPreviewAdapter adapter;
    adapter.attachViews(&list, &tree);
    adapter.setCurrentDirectoryUrl(base);
    verify(adapter.isEnabled(), "6. previews default ON");

    QPixmap pix(32, 32);
    pix.fill(Qt::red);
    list.model()->setData(idx, pix, directory_view_detail::PreviewPixmapRole);
    verify(idx.data(directory_view_detail::PreviewPixmapRole).isValid(), "7. preview present");

    const int rowsBefore = list.model()->rowCount();
    adapter.setEnabled(false);
    verify(!adapter.isEnabled(), "8. toggle OFF");
    verify(!idx.data(directory_view_detail::PreviewPixmapRole).isValid(), "9. OFF clears preview role");
    verify(!idx.data(Qt::DecorationRole).value<QIcon>().isNull(), "10. canonical icon preserved");
    verify(list.model()->rowCount() == rowsBefore && idx.isValid(), "11. items not recreated on OFF");
    verify(!adapter.controller().isRunning(), "12. no active job after OFF");

    adapter.scheduleUpdate();
    adapter.invalidatePreviews(); // F5 while OFF
    adapter.updatePreviewsNow();
    verify(!adapter.controller().isRunning(), "13. F5 while OFF generates nothing");
    verify(!idx.data(directory_view_detail::PreviewPixmapRole).isValid(), "14. F5 while OFF keeps role empty");

    adapter.setEnabled(false);
    verify(!adapter.isEnabled(), "15. repeated OFF idempotent");
    adapter.setEnabled(true);
    verify(adapter.isEnabled(), "16. toggle ON");
    verify(list.model()->rowCount() == rowsBefore, "17. items not recreated on ON");
    adapter.setEnabled(true);
    verify(adapter.isEnabled(), "18. repeated ON idempotent");
    adapter.invalidatePreviews();
    verify(adapter.isEnabled(), "19. F5 while ON allowed");
    for (int i = 0; i < 5; ++i) { adapter.setEnabled(false); adapter.setEnabled(true); }
    verify(adapter.isEnabled(), "20. rapid toggling stable");

    // Panes own independent adapters.
    PrimaryBrowserPane primary;
    SplitBrowserPane split;
    primary.previewAdapter()->setEnabled(false);
    verify(split.previewAdapter()->isEnabled(), "21. primary OFF does not affect split");
    split.previewAdapter()->setEnabled(false);
    split.setDisplayOptions(false, true);
    verify(split.previewAdapter()->isEnabled() && !primary.previewAdapter()->isEnabled(),
           "22. split display options toggle only split adapter");

    // Hard no-relisting seam: the counter changes only in startListing().
    const quint64 splitListings = split.listingStartCount();
    split.setPreviewsEnabled(false);
    split.setPreviewsEnabled(true);
    verify(split.listingStartCount() == splitListings, "23. split toggle never calls startListing");
    const quint64 primaryListings = primary.listingStartCount();
    primary.previewAdapter()->setEnabled(true);
    primary.previewAdapter()->setEnabled(false);
    verify(primary.listingStartCount() == primaryListings, "24. primary adapter toggle never calls startListing");

    // Per-location persistence is part of DirectoryViewProfile, defaulting ON.
    QSettings().clear();
    verify(DirectoryViewSettings::globalDefault().previewsEnabled, "25. first-run profile defaults previews ON");
    DirectoryViewProfile profile = DirectoryViewSettings::globalDefault();
    profile.previewsEnabled = false;
    DirectoryViewSettings::saveExplicitProfile(base, profile);
    QSettings().sync();
    verify(!DirectoryViewSettings::resolveProfile(base).previewsEnabled, "26. OFF persists per location");
    profile.previewsEnabled = true;
    DirectoryViewSettings::saveExplicitProfile(base, profile);
    QSettings().sync();
    verify(DirectoryViewSettings::resolveProfile(base).previewsEnabled, "27. ON persists per location");
    const QUrl other = QUrl::fromLocalFile(tempDir.filePath(QStringLiteral("other")));
    verify(DirectoryViewSettings::resolveProfile(other).previewsEnabled, "28. unrelated location retains ON default");
    DirectoryViewProfile otherProfile = DirectoryViewSettings::resolveProfile(other);
    otherProfile.previewsEnabled = false;
    DirectoryViewSettings::saveExplicitProfile(other, otherProfile);
    verify(!DirectoryViewSettings::resolveProfile(other).previewsEnabled
               && DirectoryViewSettings::resolveProfile(base).previewsEnabled,
           "29. locations persist independently across navigation");
    const QUrl searchUrl(QStringLiteral("thispcsearch:/?q=stage6"));
    DirectoryViewProfile searchProfile = DirectoryViewSettings::resolveProfile(searchUrl);
    verify(searchProfile.previewsEnabled, "30. Search follows default profile contract");
    searchProfile.previewsEnabled = false;
    DirectoryViewSettings::saveExplicitProfile(searchUrl, searchProfile);
    verify(!DirectoryViewSettings::resolveProfile(searchUrl).previewsEnabled,
           "31. Search preview preference persists through the same profile API");

    // Real visible-item seam with a backend factory that cannot perform I/O.
    QFile imageFile(png.toLocalFile());
    verify(imageFile.open(QIODevice::WriteOnly), "32. local preview fixture created");
    imageFile.write("not-decoded-by-test-factory");
    imageFile.close();
    list.show();
    app.processEvents();
    int batches = 0;
    QSize lastPhysicalSize;
    adapter.controller().setPreviewJobFactoryForTesting(
        [](const KFileItemList &, const QSize &, const QStringList *) -> KIO::PreviewJob * { return nullptr; });
    QObject::connect(&adapter.controller(), &PreviewController::previewBatchRequested,
                     [&](const QList<QUrl> &, const QSize &size, quint64) {
                         ++batches;
                         lastPhysicalSize = size;
                     });
    adapter.setEnabled(true);
    list.setIconSize(QSize(32, 32));
    adapter.updatePreviewsNow();
    verify(batches == 1, "33. ON schedules one visible eligible batch");
    verify(lastPhysicalSize.width() >= 32 && lastPhysicalSize.height() >= 32,
           "34. request uses physical icon size");
    const int beforeResize = batches;
    list.setIconSize(QSize(80, 80));
    adapter.updatePreviewsNow();
    verify(batches == beforeResize + 1, "35. icon size change while ON requests a new preview");
    verify(lastPhysicalSize.width() >= 80 && lastPhysicalSize.height() >= 80,
           "36. larger icon requests a larger physical preview");
    adapter.setEnabled(false);
    const int disabledBatches = batches;
    list.setIconSize(QSize(128, 128));
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches, "37. icon size change while OFF requests nothing");
    adapter.invalidatePreviews();
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches, "38. F5 invalidation while OFF requests nothing");
    adapter.setEnabled(true);
    adapter.invalidatePreviews();
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches + 1, "39. F5 invalidation while ON reschedules");
    adapter.setCurrentDirectoryUrl(QUrl(QStringLiteral("sftp://host/path")));
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches + 1, "40. remote URL never schedules a preview fetch");
    adapter.setCurrentDirectoryUrl(searchUrl);
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches + 2,
           "41. Search schedules its visible local result through the same adapter contract");
    list.model()->setData(idx, QStringLiteral("sftp://host/remote.png"),
                          directory_view_detail::UrlRole);
    adapter.updatePreviewsNow();
    verify(batches == disabledBatches + 2,
           "42. Search never fetches a remote result preview");

    // Four view modes retain the canonical model and do not relist.
    SplitBrowserPane modePane;
    const quint64 modeListings = modePane.listingStartCount();
    for (int mode = 0; mode < 4; ++mode) {
        modePane.setViewMode(mode, false);
        verify(modePane.viewMode() == mode, qPrintable(QStringLiteral("42. view mode %1 applied").arg(mode)));
    }
    verify(modePane.listingStartCount() == modeListings, "46. Icons/List/Details/Compact changes do not relist");
    const int modeBatches = batches;
    adapter.setEnabled(false);
    for (int mode = 0; mode < 4; ++mode) {
        modePane.setViewMode(mode, false);
        adapter.updatePreviewsNow();
    }
    verify(batches == modeBatches, "47. all four modes request zero previews while OFF");

    // Full View menu -> active pane -> adapter/profile path.
    QLocale::setDefault(QLocale(QLocale::English));
    ThisPcWindow window(base, false);
    window.m_primaryPane->cancelListing();
    window.show();
    app.processEvents();
    verify(window.m_thumbnailsAction->isCheckable(), "48. View/Show Previews is checkable");
    verify(window.m_thumbnailsAction->text() == QStringLiteral("Show Previews"), "49. English label is Show Previews");
    verify(window.m_thumbnailsAction->shortcut().isEmpty(), "50. Show Previews has no unaudited shortcut");
    window.setActivePane(PaneId::Primary);
    const quint64 uiPrimaryListings = window.m_primaryPane->listingStartCount();
    window.m_thumbnailsAction->setChecked(false);
    verify(!window.m_primaryPane->previewAdapter()->isEnabled(), "51. real View action disables active Primary adapter");
    verify(window.m_primaryPane->listingStartCount() == uiPrimaryListings, "52. real Primary menu toggle has hard zero relist proof");
    verify(!DirectoryViewSettings::resolveProfile(base).previewsEnabled, "53. real Primary menu toggle persists OFF");
    window.m_thumbnailsAction->setChecked(true);
    verify(window.m_primaryPane->previewAdapter()->isEnabled(), "54. real View action re-enables Primary adapter");
    verify(DirectoryViewSettings::resolveProfile(base).previewsEnabled, "55. real Primary menu toggle persists ON");

    window.setSplitViewEnabled(true);
    window.m_splitPane->cancelListing();
    window.setActivePane(PaneId::Split);
    const quint64 uiSplitListings = window.m_splitPane->listingStartCount();
    window.m_thumbnailsAction->setChecked(false);
    verify(!window.m_splitPane->previewAdapter()->isEnabled(), "56. real View action disables active Split adapter");
    verify(window.m_primaryPane->previewAdapter()->isEnabled(), "57. Split toggle leaves Primary independent");
    verify(window.m_splitPane->listingStartCount() == uiSplitListings, "58. real Split menu toggle has hard zero relist proof");
    window.setActivePane(PaneId::Primary);
    verify(window.m_thumbnailsAction->isChecked(), "59. clicking Primary synchronizes View check state");
    window.setActivePane(PaneId::Split);
    verify(!window.m_thumbnailsAction->isChecked(), "60. clicking Split synchronizes View check state");
    window.swapSplitPanes();
    window.m_primaryPane->cancelListing();
    window.m_splitPane->cancelListing();
    window.updateFileActionStates();
    verify(window.m_thumbnailsAction->isChecked()
               == (window.m_activePane == PaneId::Split
                       ? window.m_splitPane->previewsEnabled() : window.m_thumbnailsEnabled),
           "61. swap synchronizes check state to active pane");
    window.setSplitViewEnabled(false);
    verify(window.m_activePane == PaneId::Primary
               && window.m_thumbnailsAction->isChecked() == window.m_thumbnailsEnabled,
           "62. disabling split returns View state to Primary");

    QLocale::setDefault(QLocale(QLocale::Polish));
    ThisPcWindow polishWindow(kThisPcUrl, false);
    verify(polishWindow.m_thumbnailsAction->text() == QString::fromUtf8("Pokaż podglądy"),
           "63. Polish label is Pokaż podglądy");
    QLocale::setDefault(QLocale(QLocale::English));

    // Explicit override remains available and neutral defaults never require another app's rc.
    PreviewController overrideController;
    overrideController.setEnabledPlugins({QStringLiteral("imagethumbnail")});
    verify(overrideController.enabledPlugins() == QStringList{QStringLiteral("imagethumbnail")},
           "64. explicit plugin override API remains intact");
    verify(PreviewController::queryDefaultPlugins() == KIO::PreviewJob::defaultPlugins(),
           "65. runtime plugin policy is exactly KIO defaultPlugins");

    qDebug("preview_settings PASS: %d assertions", checks);
    return 0;
}
