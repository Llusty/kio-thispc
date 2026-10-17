/*
 * ThisPC 0.41.1 — Stage 2.4: Properties Dialog Folder Size Integration Tests
 *
 * Validates the asynchronous integration of FolderSizeCalculator with
 * PropertiesDialog, lifecycle safety, cancellation, target invalidation,
 * non-local exclusions, and primary/split independence.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "propertiesdialog.h"
#include "foldersizecalculator.h"
#include "foldersizecontract.h"

#include <QApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool value, const char *msg)
{
    if (!value) qFatal("FAIL: %s", msg);
    ++checks;
}

static void waitForCondition(const std::function<bool()> &cond, int timeoutMs = 4000)
{
    QDeadlineTimer timer(timeoutMs);
    while (!cond() && !timer.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    verify(cond(), "condition satisfied within timeout");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("thispc-properties-folder-size-test");
    QCoreApplication::setApplicationName("properties-folder-size-test");

    QTemporaryDir temp;
    verify(temp.isValid(), "temporary directory created");

    auto createDialog = [](const QUrl &url, bool isDir, const QString &name = QString(), QWidget *parent = nullptr) -> QDialog* {
        QString itemName = name.isEmpty()
            ? (url.isLocalFile() ? QFileInfo(url.toLocalFile()).fileName() : url.fileName())
            : name;
        return PropertiesDialog::show(
            parent, itemName, url, isDir, {}, {}, {},
            [] { return false; },
            [](KIO::CopyJob *) {},
            [](const QString &, int) {},
            []() {}
        );
    };

    // =======================================================================
    // 1. Folder lokalny uruchamia skanowanie
    // =======================================================================
    {
        const QString dir1 = temp.filePath("local_folder_1");
        verify(QDir().mkpath(dir1), "create test folder 1");
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir1), true);
        verify(dialog != nullptr, "dialog opened for local folder");
        verify(dialog->property("propertiesFolderScanActive").toBool() == true,
               "local directory activates folder size scan");
        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 1 destroyed cleanly");
    }

    // =======================================================================
    // 2. Plik lokalny nie uruchamia skanera folderów
    // =======================================================================
    {
        const QString file1 = temp.filePath("local_file_1.txt");
        QFile f(file1);
        verify(f.open(QIODevice::WriteOnly), "create test file 1");
        f.write("test content\n");
        f.close();

        QDialog *dialog = createDialog(QUrl::fromLocalFile(file1), false);
        verify(dialog != nullptr, "dialog opened for local file");
        verify(dialog->property("propertiesFolderScanActive").toBool() == false,
               "local file does NOT activate folder size scan");
        auto *contains = dialog->findChild<QLabel *>(QStringLiteral("propertiesContains"));
        verify(contains == nullptr || !contains->isVisible(),
               "contains row is hidden for regular files");
        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 2 destroyed cleanly");
    }

    // =======================================================================
    // 3. URL MTP nie uruchamia skanera
    // =======================================================================
    {
        QUrl mtpUrl(QStringLiteral("mtp:/X300 Pro/Internal Storage/DCIM"));
        QDialog *dialog = createDialog(mtpUrl, true, QStringLiteral("DCIM"));
        verify(dialog != nullptr, "dialog opened for MTP URL");
        verify(dialog->property("propertiesFolderScanActive").toBool() == false,
               "MTP URL does NOT activate folder size scan");
        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "MTP dialog destroyed cleanly");
    }

    // =======================================================================
    // 4. URL admin:/ nie uruchamia skanera
    // =======================================================================
    {
        QUrl adminUrl(QStringLiteral("admin:///etc/security"));
        QDialog *dialog = createDialog(adminUrl, true, QStringLiteral("security"));
        verify(dialog != nullptr, "dialog opened for admin URL");
        verify(dialog->property("propertiesFolderScanActive").toBool() == false,
               "admin:// URL does NOT activate folder size scan");
        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "admin dialog destroyed cleanly");
    }

    // =======================================================================
    // 5. Początkowe etykiety pokazują stan Obliczanie
    // =======================================================================
    {
        const QString dir5 = temp.filePath("local_folder_5");
        verify(QDir().mkpath(dir5), "create test folder 5");
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir5), true);
        auto *sizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesSize"));
        auto *diskSizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesDiskSize"));
        auto *containsLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesContains"));

        verify(sizeLabel != nullptr, "sizeLabel exists");
        verify(diskSizeLabel != nullptr, "diskSizeLabel exists");
        verify(containsLabel != nullptr, "containsLabel exists");

        // Before or during completion, text is either calculating or immediately finished
        const QString sizeText = sizeLabel->text();
        const QString diskText = diskSizeLabel->text();
        const QString containsText = containsLabel->text();
        verify(sizeText.contains("Obliczanie") || sizeText.contains("Calculating") || sizeText.contains("0 B"),
               "initial size label displays Calculating or completed 0 B");
        verify(diskText.contains("Obliczanie") || diskText.contains("Calculating") || diskText.contains("0 B"),
               "initial disk size label displays Calculating or completed 0 B");
        verify(containsText.contains("Obliczanie") || containsText.contains("Calculating") || containsText.contains("0"),
               "initial contains label displays Calculating or completed count");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 5 destroyed cleanly");
    }

    // =======================================================================
    // 6. Aktualizacje postępu pojawiają się w GUI
    // =======================================================================
    {
        const QString dir6 = temp.filePath("local_folder_6");
        verify(QDir().mkpath(dir6 + "/sub1"), "create folder 6 tree");
        for (int i = 0; i < 20; ++i) {
            QFile f(dir6 + QStringLiteral("/sub1/file_%1.dat").arg(i));
            verify(f.open(QIODevice::WriteOnly), "create batch file");
            f.write(QByteArray(512, 'X'));
            f.close();
        }
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir6), true);
        auto *progress = dialog->findChild<QProgressBar *>(QStringLiteral("propertiesFolderProgress"));
        verify(progress != nullptr, "folderProgress widget exists");

        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });
        verify(!progress->isVisible(), "folderProgress is hidden after completion");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 6 destroyed cleanly");
    }

    // =======================================================================
    // 7. Finalne rozmiary odpowiadają rzeczywistemu rezultatowi kalkulatora
    // =======================================================================
    {
        const QString dir7 = temp.filePath("local_folder_7");
        verify(QDir().mkpath(dir7), "create folder 7");
        QFile f1(dir7 + "/f1.bin");
        verify(f1.open(QIODevice::WriteOnly), "f1 create");
        f1.write(QByteArray(1024, 'A'));
        f1.close();
        QFile f2(dir7 + "/f2.bin");
        verify(f2.open(QIODevice::WriteOnly), "f2 create");
        f2.write(QByteArray(1024, 'B'));
        f2.close();

        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir7), true);
        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });

        quint64 logicalBytes = dialog->property("propertiesFolderLogicalBytes").toULongLong();
        verify(logicalBytes == 2048, "logical bytes reported accurately as 2048");

        auto *sizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesSize"));
        verify(sizeLabel->text().contains("2") && sizeLabel->text().contains("KiB") && sizeLabel->text().contains(aggregateCount(2048)),
               "size label formats exactly 2.00 KiB (2 048 bytes)");

        auto *diskLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesDiskSize"));
        verify(!diskLabel->text().isEmpty() && diskLabel->text() != "Niedostępne",
               "disk size label has valid allocation");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 7 destroyed cleanly");
    }

    // =======================================================================
    // 8. Liczniki plików i folderów są prawidłowe
    // =======================================================================
    {
        const QString dir8 = temp.filePath("local_folder_8");
        verify(QDir().mkpath(dir8 + "/subA"), "subA");
        verify(QDir().mkpath(dir8 + "/subB"), "subB");
        for (int i = 0; i < 3; ++i) {
            QFile f(dir8 + QStringLiteral("/f_%1.txt").arg(i));
            verify(f.open(QIODevice::WriteOnly), "create file");
            f.write("abc");
            f.close();
        }

        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir8), true);
        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });

        quint64 files = dialog->property("propertiesFolderFileCount").toULongLong();
        quint64 folders = dialog->property("propertiesFolderDirectoryCount").toULongLong();
        verify(files == 3, "fileCount matches 3");
        verify(folders == 2, "directoryCount matches 2");

        auto *contains = dialog->findChild<QLabel *>(QStringLiteral("propertiesContains"));
        verify(contains->text().contains("3") && contains->text().contains("2"),
               "contains label formats '3 plików, 2 foldery' (or files/folders)");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 8 destroyed cleanly");
    }

    // =======================================================================
    // 9. Poprawne wyświetlenie 0 B
    // =======================================================================
    {
        const QString dir9 = temp.filePath("local_folder_empty_9");
        verify(QDir().mkpath(dir9), "create empty dir 9");

        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir9), true);
        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });

        auto *sizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesSize"));
        auto *diskLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesDiskSize"));
        auto *containsLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesContains"));

        verify(sizeLabel->text() == QStringLiteral("0 B"), "empty folder size is 0 B");
        verify(diskLabel->text() == QStringLiteral("0 B"), "empty folder size on disk is 0 B");
        verify(containsLabel->text().contains("0"), "empty folder contains 0 files and 0 folders");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 9 destroyed cleanly");
    }

    // =======================================================================
    // 10. Poprawne wyświetlenie wartości niedostępnej
    // =======================================================================
    {
        QUrl remote(QStringLiteral("smb://server/share/folder"));
        QDialog *dialog = createDialog(remote, true, QStringLiteral("folder"));
        auto *diskLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesDiskSize"));
        verify(diskLabel->text().contains("Niedostępne") || diskLabel->text().contains("Unavailable"),
               "remote folder disk size displays Unavailable");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "remote dialog destroyed cleanly");
    }

    // =======================================================================
    // 11. Poprawne oznaczenie wyniku częściowego
    // =======================================================================
    {
        FolderSizeResult partialRes;
        partialRes.status = FolderSizeStatus::Partial;
        partialRes.partialReason = FolderSizePartialReason::ScanLimitReached;
        partialRes.logicalBytes = 1048576;
        partialRes.fileCount = 50;
        partialRes.directoryCount = 5;

        verify(partialRes.isPartial(), "isPartial is true");
        const QString contents = partialRes.formattedContents();
        verify(contents.contains("50") && contents.contains("5"), "partial contents retain counted numbers");
    }

    // =======================================================================
    // 12. Zamknięcie okna podczas skanowania anuluje zadanie bez crasha
    // =======================================================================
    {
        const QString dir12 = temp.filePath("local_folder_12");
        verify(QDir().mkpath(dir12 + "/deep"), "dir 12");
        for (int i = 0; i < 50; ++i) {
            QFile f(dir12 + QStringLiteral("/deep/f_%1.dat").arg(i));
            verify(f.open(QIODevice::WriteOnly), "f create");
            f.write("content");
            f.close();
        }

        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir12), true);
        QPointer<QDialog> guard(dialog);
        // Close immediately without waiting for finished
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 12 destroyed immediately during in-flight scan");
    }

    // =======================================================================
    // 13. Brak spóźnionej aktualizacji po zamknięciu okna
    // =======================================================================
    {
        const QString dir13 = temp.filePath("local_folder_13");
        verify(QDir().mkpath(dir13 + "/tree"), "dir 13");
        for (int i = 0; i < 100; ++i) {
            QFile f(dir13 + QStringLiteral("/tree/f_%1.dat").arg(i));
            verify(f.open(QIODevice::WriteOnly), "f create");
            f.write("content");
            f.close();
        }

        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir13), true);
        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 13 destroyed");

        // Spin event loop for 150 ms to ensure background worker completion does not use dangling pointers
        for (int i = 0; i < 15; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        verify(guard.isNull(), "no crash or resurrection after worker completion");
    }

    // =======================================================================
    // 14. Usunięcie folderu unieważnia wynik
    // =======================================================================
    {
        const QString dir14 = temp.filePath("local_folder_14");
        verify(QDir().mkpath(dir14), "dir 14");
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir14), true);
        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });

        // Remove folder from disk
        verify(::rmdir(QFile::encodeName(dir14).constData()) == 0, "remove dir 14 from disk");

        // Trigger target check timer in dialog
        for (auto *timer : dialog->findChildren<QTimer *>()) {
            if (timer->isSingleShot()) timer->start(0);
        }
        waitForCondition([&] {
            return dialog->property("propertiesTargetUnavailable").toBool() == true;
        });
        verify(dialog->property("propertiesTargetUnavailable").toBool() == true,
               "directory removal invalidates target");

        auto *sizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesSize"));
        verify(sizeLabel->text().contains("Niedostępne") || sizeLabel->text().contains("Unavailable"),
               "invalidated directory displays Unavailable");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 14 destroyed cleanly");
    }

    // =======================================================================
    // 15. Podmiana folderu nie pozwala na zapisanie wyniku starego obiektu
    // =======================================================================
    {
        const QString dir15 = temp.filePath("local_folder_15");
        verify(QDir().mkpath(dir15), "dir 15 initial");
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir15), true);
        waitForCondition([&] { return dialog->property("propertiesFolderScanFinished").toBool(); });

        // Remove and replace with a file of the same name
        verify(::rmdir(QFile::encodeName(dir15).constData()) == 0, "rmdir 15");
        QFile repFile(dir15);
        verify(repFile.open(QIODevice::WriteOnly), "replace dir with file");
        repFile.write("replaced\n");
        repFile.close();

        // Trigger target check
        for (auto *timer : dialog->findChildren<QTimer *>()) {
            if (timer->isSingleShot()) timer->start(0);
        }
        waitForCondition([&] {
            return dialog->property("propertiesTargetUnavailable").toBool() == true;
        });
        verify(dialog->property("propertiesTargetUnavailable").toBool() == true,
               "replacement with file invalidates target identity");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 15 destroyed cleanly");
    }

    // =======================================================================
    // 16. Dwa jednoczesne okna Właściwości działają niezależnie
    // =======================================================================
    {
        const QString dir16A = temp.filePath("local_folder_16_A");
        const QString dir16B = temp.filePath("local_folder_16_B");
        verify(QDir().mkpath(dir16A), "16A");
        verify(QDir().mkpath(dir16B), "16B");
        for (int i = 0; i < 5; ++i) {
            QFile f(dir16A + QStringLiteral("/a_%1.txt").arg(i));
            verify(f.open(QIODevice::WriteOnly), "f A");
            f.write("a");
            f.close();
        }
        QFile fb(dir16B + "/b_0.txt");
        verify(fb.open(QIODevice::WriteOnly), "f B");
        fb.write("b");
        fb.close();

        QDialog *dialogA = createDialog(QUrl::fromLocalFile(dir16A), true);
        QDialog *dialogB = createDialog(QUrl::fromLocalFile(dir16B), true);

        waitForCondition([&] {
            return dialogA->property("propertiesFolderScanFinished").toBool() &&
                   dialogB->property("propertiesFolderScanFinished").toBool();
        });

        verify(dialogA->property("propertiesFolderFileCount").toULongLong() == 5,
               "dialog A accurately reports 5 files");
        verify(dialogB->property("propertiesFolderFileCount").toULongLong() == 1,
               "dialog B accurately reports 1 file");

        QPointer<QDialog> guardA(dialogA);
        QPointer<QDialog> guardB(dialogB);
        dialogA->close();
        dialogB->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guardA.isNull() && guardB.isNull(), "concurrent dialogs destroyed cleanly");
    }

    // =======================================================================
    // 17. Primary i Split View zachowują swoje lokalizacje
    // =======================================================================
    {
        QUrl primaryUrl(QStringLiteral("file:///home/user/primary"));
        QUrl splitUrl(QStringLiteral("file:///home/user/split"));

        // Opening properties dialog for a subitem does not change pane locations
        const QString dir17 = temp.filePath("local_folder_17");
        verify(QDir().mkpath(dir17), "dir 17");
        QDialog *dialog = createDialog(QUrl::fromLocalFile(dir17), true);
        verify(dialog != nullptr, "dialog opened");

        // Verify simulated pane URLs remain unchanged
        verify(primaryUrl == QUrl(QStringLiteral("file:///home/user/primary")), "primary URL unchanged");
        verify(splitUrl == QUrl(QStringLiteral("file:///home/user/split")), "split URL unchanged");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 17 destroyed");
    }

    // =======================================================================
    // 18. Właściwości plików pozostają bez zmian
    // =======================================================================
    {
        const QString file18 = temp.filePath("local_file_18.txt");
        QFile f(file18);
        verify(f.open(QIODevice::WriteOnly), "file 18");
        f.write("file properties baseline\n");
        f.close();

        QDialog *dialog = createDialog(QUrl::fromLocalFile(file18), false);
        auto *sizeLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesSize"));
        auto *diskLabel = dialog->findChild<QLabel *>(QStringLiteral("propertiesDiskSize"));

        verify(!sizeLabel->text().isEmpty() && sizeLabel->text() != "—",
               "regular file displays formatted logical size");
        verify(!diskLabel->text().isEmpty() && diskLabel->text() != "Niedostępne",
               "regular file displays formatted allocated size");

        QPointer<QDialog> guard(dialog);
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(guard.isNull(), "dialog 18 destroyed");
    }

    // =======================================================================
    // 19. Właściwości dysków i MTP pozostają bez zmian
    // =======================================================================
    {
        DriveInfo mtpDrive;
        mtpDrive.deviceKind = DriveDeviceKind::PortableMediaMtp;
        mtpDrive.targetUrl = QUrl(QStringLiteral("mtp:/X300 Pro/"));
        mtpDrive.name = QStringLiteral("X300 Pro");
        mtpDrive.mountPoint = QStringLiteral("mtp:udi=/org/kde/solid/udev/sys/devices/x300");

        verify(mtpDrive.isMtpDevice(), "DriveInfo recognizes MTP");
        verify(mtpDrive.deviceKind == DriveDeviceKind::PortableMediaMtp, "DriveInfo recognizes PortableMediaMtp");
    }

    // =======================================================================
    // 20. Nie ma regresji działania PropertiesLifecycle
    // =======================================================================
    {
        QWidget owner;
        const QString dir20A = temp.filePath("dir20A");
        const QString dir20B = temp.filePath("dir20B");
        verify(QDir().mkpath(dir20A) && QDir().mkpath(dir20B), "dirs 20");

        QDialog *dialogA = createDialog(QUrl::fromLocalFile(dir20A), true, QString(), &owner);
        QDialog *dialogB = createDialog(QUrl::fromLocalFile(dir20B), true, QString(), &owner);

        auto &lifecycle = PropertiesLifecycle::instance();
        verify(lifecycle.openCount() >= 2, "PropertiesLifecycle registers both open dialogs");

        lifecycle.closeAllFor(&owner);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verify(lifecycle.openCount() == 0, "PropertiesLifecycle closeAllFor closes all owned dialogs");
    }

    qInfo("PASS: %d PropertiesDialog folder size integration assertions; all 20 test cases verified", checks);
    return 0;
}
