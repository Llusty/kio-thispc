/*
 * Properties dialog and permission editing support.
 *
 * Extracted during the 0.21.0 architecture refactor. The dialog keeps the
 * existing KIO/admin:// behavior while the main window only supplies the
 * window-level callbacks it owns.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "batchrenamerecovery.h"

#include "browsercommon.h"

#include <KIO/ChmodJob>
#include <KIO/CopyJob>
#include <KIO/StatJob>
#include <KFileItem>
#include <KJob>

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QStorageInfo>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

class PropertiesDialog final
{
public:
    using EnsureAdminProtocol = std::function<bool()>;
    using RecordCopyJob = std::function<void(KIO::CopyJob *)>;
    using ShowStatus = std::function<void(const QString &, int)>;
    using RefreshView = std::function<void()>;

    static void show(
        QWidget *parent,
        const QString &name,
        const QUrl &url,
        bool isDir,
        const QString &typeText,
        const QString &sizeText,
        const QString &modifiedText,
        const EnsureAdminProtocol &ensureAdminProtocol,
        const RecordCopyJob &recordCopyJob,
        const ShowStatus &showStatus,
        const RefreshView &refreshView)
    {
        KFileItem fileItem(
            url,
            KFileItem::NormalMimeTypeDetermination);

        QDialog dialog(parent);
        dialog.setWindowTitle(
            trLocal("Właściwości — ", "Properties — ") + name);
        dialog.resize(630, 625);

        QUrl workingUrl = url;
        QString workingName = name;

        const QString physicalPath =
            localPathForFileOrAdmin(url);
        const QString fileSystem =
            filesystemTypeForUrl(url);
        const bool permissionBehaviorMayDependOnMount =
            filesystemMayUseMountControlledPermissions(
                fileSystem);
        const bool readOnlyFileSystem =
            filesystemIsReadOnly(url);

        auto *outer = new QVBoxLayout(&dialog);

        auto *header = new QHBoxLayout;
        auto *iconLabel = new QLabel(&dialog);
        QIcon icon = themedIcon(
            fileItem.iconName().isEmpty()
                ? (isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral("text-x-generic"))
                : fileItem.iconName());
        iconLabel->setPixmap(icon.pixmap(64, 64));
        iconLabel->setFixedSize(72, 72);
        iconLabel->setAlignment(Qt::AlignCenter);

        auto *headerName = new QLabel(name, &dialog);
        QFont headerFont = headerName->font();
        headerFont.setPointSize(
            headerFont.pointSize() + 3);
        headerFont.setBold(true);
        headerName->setFont(headerFont);
        headerName->setWordWrap(true);

        header->addWidget(iconLabel);
        header->addWidget(headerName, 1);
        outer->addLayout(header);

        auto *tabs = new QTabWidget(&dialog);
        outer->addWidget(tabs, 1);

        auto makeValueLabel =
            [&dialog](const QString &value) {
            auto *label = new QLabel(
                value.isEmpty()
                    ? QStringLiteral("—")
                    : value,
                &dialog);
            label->setTextInteractionFlags(
                Qt::TextSelectableByMouse);
            label->setWordWrap(true);
            return label;
        };

        // --------------------------------------------------------------
        // General
        // --------------------------------------------------------------
        auto *general = new QWidget(tabs);
        auto *generalForm =
            new QFormLayout(general);
        generalForm->setFieldGrowthPolicy(
            QFormLayout::AllNonFixedFieldsGrow);

        auto *nameEdit =
            new QLineEdit(name, general);
        nameEdit->setReadOnly(
            !(url.isLocalFile()
              || isAdminUrl(url)));
        generalForm->addRow(
            trLocal("Nazwa:", "Name:"),
            nameEdit);

        QString mimeName = fileItem.mimetype();
        QString friendlyType =
            fileItem.mimeComment();

        if (friendlyType.isEmpty()) {
            friendlyType = typeText;
        }

        if (mimeName.isEmpty() && !isDir) {
            QMimeDatabase db;
            mimeName =
                db.mimeTypeForFile(
                    physicalPath.isEmpty()
                        ? name
                        : physicalPath,
                    QMimeDatabase::MatchExtension)
                    .name();
        }

        QString actualSize = sizeText;
        if (!isDir) {
            actualSize = formatFileSize(
                static_cast<qint64>(
                    fileItem.size()),
                false);
        }

        const QString modified =
            fileItem.time(
                KFileItem::ModificationTime)
                .isValid()
                ? QLocale().toString(
                    fileItem.time(
                        KFileItem::ModificationTime),
                    QLocale::LongFormat)
                : modifiedText;

        const QDateTime creationTime =
            fileItem.time(
                KFileItem::CreationTime);

        const QString created =
            creationTime.isValid()
            && creationTime.date().year() > 1971
                ? QLocale().toString(
                    creationTime,
                    QLocale::LongFormat)
                : trLocal(
                    "Niedostępne",
                    "Unavailable");

        const QString accessed =
            fileItem.time(
                KFileItem::AccessTime).isValid()
                ? QLocale().toString(
                    fileItem.time(
                        KFileItem::AccessTime),
                    QLocale::LongFormat)
                : QStringLiteral("—");

        generalForm->addRow(
            trLocal("Typ:", "Type:"),
            makeValueLabel(friendlyType));
        generalForm->addRow(
            QStringLiteral("MIME:"),
            makeValueLabel(mimeName));
        generalForm->addRow(
            trLocal("Rozmiar:", "Size:"),
            makeValueLabel(actualSize));
        generalForm->addRow(
            trLocal("Zmodyfikowano:", "Modified:"),
            makeValueLabel(modified));
        generalForm->addRow(
            trLocal("Utworzono:", "Created:"),
            makeValueLabel(created));
        generalForm->addRow(
            trLocal("Ostatni dostęp:", "Accessed:"),
            makeValueLabel(accessed));

        if (!physicalPath.isEmpty()) {
            const QFileInfo info(physicalPath);

            generalForm->addRow(
                trLocal("Lokalizacja:", "Location:"),
                makeValueLabel(
                    info.absolutePath()));
            generalForm->addRow(
                trLocal(
                    "Rozszerzenie:",
                    "Extension:"),
                makeValueLabel(info.suffix()));
            generalForm->addRow(
                trLocal("Właściciel:", "Owner:"),
                makeValueLabel(info.owner()));
            generalForm->addRow(
                trLocal("Grupa:", "Group:"),
                makeValueLabel(info.group()));
            generalForm->addRow(
                trLocal(
                    "System plików:",
                    "Filesystem:"),
                makeValueLabel(
                    fileSystem.isEmpty()
                        ? trLocal(
                            "Niedostępne",
                            "Unavailable")
                        : fileSystem));

            QImageReader reader(physicalPath);
            const QSize imageSize =
                reader.size();

            if (imageSize.isValid()) {
                generalForm->addRow(
                    trLocal(
                        "Wymiary obrazu:",
                        "Image dimensions:"),
                    makeValueLabel(
                        QStringLiteral("%1 × %2 px")
                            .arg(
                                imageSize.width())
                            .arg(
                                imageSize.height())));
            }
        }

        generalForm->addRow(
            trLocal("Adres:", "Address:"),
            makeValueLabel(
                urlForDisplay(url)));

        tabs->addTab(
            general,
            themedIcon(
                QStringLiteral(
                    "document-properties")),
            trLocal("Ogólne", "General"));

        // --------------------------------------------------------------
        // Permissions
        // --------------------------------------------------------------
        auto *permissionsPage =
            new QWidget(tabs);
        auto *permissionsLayout =
            new QVBoxLayout(permissionsPage);

        auto *permissionsInfo =
            new QLabel(permissionsPage);
        permissionsInfo->setWordWrap(true);
        permissionsLayout->addWidget(
            permissionsInfo);

        auto *adminUnlockButton =
            new QPushButton(
                themedIcon(
                    QStringLiteral(
                        "security-high")),
                trLocal(
                    "Odblokuj jako administrator",
                    "Unlock as administrator"),
                permissionsPage);
        adminUnlockButton->setVisible(false);
        permissionsLayout->addWidget(
            adminUnlockButton,
            0,
            Qt::AlignLeft);

        auto *permissionsGroup =
            new QGroupBox(
                trLocal(
                    "Uprawnienia POSIX",
                    "POSIX permissions"),
                permissionsPage);

        auto *permGrid =
            new QGridLayout(
                permissionsGroup);

        permGrid->addWidget(
            new QLabel(
                QString(),
                permissionsGroup),
            0, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Odczyt", "Read"),
                permissionsGroup),
            0, 1);
        permGrid->addWidget(
            new QLabel(
                trLocal("Zapis", "Write"),
                permissionsGroup),
            0, 2);
        permGrid->addWidget(
            new QLabel(
                trLocal(
                    "Wykonanie",
                    "Execute"),
                permissionsGroup),
            0, 3);

        permGrid->addWidget(
            new QLabel(
                trLocal(
                    "Właściciel",
                    "Owner"),
                permissionsGroup),
            1, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Grupa", "Group"),
                permissionsGroup),
            2, 0);
        permGrid->addWidget(
            new QLabel(
                trLocal("Inni", "Others"),
                permissionsGroup),
            3, 0);

        auto *userRead =
            new QCheckBox(permissionsGroup);
        auto *userWrite =
            new QCheckBox(permissionsGroup);
        auto *userExec =
            new QCheckBox(permissionsGroup);
        auto *groupRead =
            new QCheckBox(permissionsGroup);
        auto *groupWrite =
            new QCheckBox(permissionsGroup);
        auto *groupExec =
            new QCheckBox(permissionsGroup);
        auto *otherRead =
            new QCheckBox(permissionsGroup);
        auto *otherWrite =
            new QCheckBox(permissionsGroup);
        auto *otherExec =
            new QCheckBox(permissionsGroup);

        permGrid->addWidget(
            userRead, 1, 1);
        permGrid->addWidget(
            userWrite, 1, 2);
        permGrid->addWidget(
            userExec, 1, 3);
        permGrid->addWidget(
            groupRead, 2, 1);
        permGrid->addWidget(
            groupWrite, 2, 2);
        permGrid->addWidget(
            groupExec, 2, 3);
        permGrid->addWidget(
            otherRead, 3, 1);
        permGrid->addWidget(
            otherWrite, 3, 2);
        permGrid->addWidget(
            otherExec, 3, 3);

        permissionsLayout->addWidget(
            permissionsGroup);

        auto *recursivePermissions =
            new QCheckBox(
                trLocal(
                    "Zastosuj zmiany do wszystkich podkatalogów i ich zawartości",
                    "Apply changes to all subfolders and their contents"),
                permissionsPage);
        recursivePermissions->setVisible(isDir);
        recursivePermissions->setChecked(false);
        recursivePermissions->setToolTip(
            trLocal(
                "Zmienia prawa istniejących elementów wewnątrz folderu. Dla dużych katalogów operacja może potrwać.",
                "Changes permissions of existing items inside the folder. This can take time for large directories."));
        permissionsLayout->addWidget(
            recursivePermissions);

        const QList<QCheckBox *>
            permissionBoxes = {
                userRead,
                userWrite,
                userExec,
                groupRead,
                groupWrite,
                groupExec,
                otherRead,
                otherWrite,
                otherExec
            };

        auto setPermissionBoxesEnabled =
            [&](bool enabled) {
            for (QCheckBox *box :
                 permissionBoxes) {
                box->setEnabled(enabled);
            }
            recursivePermissions->setEnabled(
                enabled && isDir);
        };

        auto setPermissionBoxesFromMode =
            [&](int mode) {
            userRead->setChecked(
                mode & 0400);
            userWrite->setChecked(
                mode & 0200);
            userExec->setChecked(
                mode & 0100);

            groupRead->setChecked(
                mode & 0040);
            groupWrite->setChecked(
                mode & 0020);
            groupExec->setChecked(
                mode & 0010);

            otherRead->setChecked(
                mode & 0004);
            otherWrite->setChecked(
                mode & 0002);
            otherExec->setChecked(
                mode & 0001);
        };

        auto modeFromPermissionBoxes =
            [&]() {
            int mode = 0;

            if (userRead->isChecked()) {
                mode |= 0400;
            }
            if (userWrite->isChecked()) {
                mode |= 0200;
            }
            if (userExec->isChecked()) {
                mode |= 0100;
            }
            if (groupRead->isChecked()) {
                mode |= 0040;
            }
            if (groupWrite->isChecked()) {
                mode |= 0020;
            }
            if (groupExec->isChecked()) {
                mode |= 0010;
            }
            if (otherRead->isChecked()) {
                mode |= 0004;
            }
            if (otherWrite->isChecked()) {
                mode |= 0002;
            }
            if (otherExec->isChecked()) {
                mode |= 0001;
            }

            return mode;
        };

        bool canEditPermissions = false;
        bool adminUnlocked =
            isAdminUrl(workingUrl);
        int currentMode = -1;
        QString permissionOwner;
        QString permissionGroup;
        QString permissionReadError;

        const bool canReadPermissions =
            readKioPermissions(
                workingUrl,
                &currentMode,
                &permissionOwner,
                &permissionGroup,
                &permissionReadError);

        if (canReadPermissions) {
            setPermissionBoxesFromMode(
                currentMode);
        }

        const bool ordinaryLocal =
            url.isLocalFile();
        const bool ownedByCurrentUser =
            ordinaryLocal
            && localEntryOwnedByCurrentUser(
                url);

        if (readOnlyFileSystem) {
            setPermissionBoxesEnabled(false);
            permissionsInfo->setText(
                trLocal(
                    "Ten system plików jest zamontowany tylko do odczytu. Uprawnień nie można zmienić.",
                    "This filesystem is mounted read-only. Permissions cannot be changed."));
        } else if (!canReadPermissions) {
            setPermissionBoxesEnabled(false);
            permissionsInfo->setText(
                isPolish()
                    ? QStringLiteral(
                        "Nie udało się odczytać uprawnień: %1")
                        .arg(permissionReadError)
                    : QStringLiteral(
                        "Could not read permissions: %1")
                        .arg(permissionReadError));

            if (ordinaryLocal && !adminUnlocked) {
                adminUnlockButton->setVisible(true);
            }
        } else if (adminUnlocked) {
            canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Uprawnienia są edytowane przez KIO admin://. Zapis może wywołać systemowe okno autoryzacji PolicyKit.",
                    "Permissions are being edited through KIO admin://. Saving may invoke the system PolicyKit authentication dialog."));
        } else if (ownedByCurrentUser) {
            canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Jesteś właścicielem tego elementu. Zmiany zostaną zapisane po użyciu „Zastosuj” lub „OK” i zweryfikowane ponownym odczytem z dysku.",
                    "You own this item. Changes are written after pressing Apply or OK and verified by reading the permissions back from disk."));
        } else {
            canEditPermissions = false;
            setPermissionBoxesEnabled(false);

            permissionsInfo->setText(
                isPolish()
                    ? QStringLiteral(
                        "Nie jesteś właścicielem tego elementu%1. Zmiana uprawnień wymaga autoryzacji administratora.")
                        .arg(
                            permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (właściciel: %1)")
                                    .arg(permissionOwner))
                    : QStringLiteral(
                        "You do not own this item%1. Changing permissions requires administrator authorization.")
                        .arg(
                            permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (owner: %1)")
                                    .arg(permissionOwner)));

            adminUnlockButton->setVisible(true);
        }

        if (ordinaryLocal
            && !adminUnlocked
            && !readOnlyFileSystem
            && !ownedByCurrentUser) {
            adminUnlockButton->setVisible(true);
        }

        permissionsLayout->addStretch(1);

        tabs->addTab(
            permissionsPage,
            themedIcon(
                QStringLiteral("security-high")),
            trLocal(
                "Uprawnienia",
                "Permissions"));

        // --------------------------------------------------------------
        // Administrator unlock
        // --------------------------------------------------------------
        QObject::connect(
            adminUnlockButton,
            &QPushButton::clicked,
            &dialog,
            [&] {
                if (!ensureAdminProtocol || !ensureAdminProtocol()) {
                    return;
                }

                const QString localPath =
                    localPathForFileOrAdmin(
                        workingUrl);

                if (localPath.isEmpty()) {
                    return;
                }

                const QUrl elevatedUrl =
                    adminUrlForLocalPath(
                        localPath);

                int elevatedMode = -1;
                QString elevatedOwner;
                QString elevatedGroup;
                QString error;

                // This stat is intentional: it verifies that the admin
                // worker is available and triggers PolicyKit authorization.
                if (!readKioPermissions(
                        elevatedUrl,
                        &elevatedMode,
                        &elevatedOwner,
                        &elevatedGroup,
                        &error)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Autoryzacja administratora",
                            "Administrator authorization"),
                        isPolish()
                            ? QStringLiteral(
                                "Nie udało się odblokować elementu przez admin://:\n%1")
                                .arg(error)
                            : QStringLiteral(
                                "Could not unlock the item through admin://:\n%1")
                                .arg(error));
                    return;
                }

                workingUrl = elevatedUrl;
                adminUnlocked = true;
                canEditPermissions = true;
                currentMode = elevatedMode;
                permissionOwner =
                    elevatedOwner;
                permissionGroup =
                    elevatedGroup;

                setPermissionBoxesFromMode(
                    elevatedMode);
                setPermissionBoxesEnabled(
                    true);
                adminUnlockButton->hide();

                permissionsInfo->setText(
                    trLocal(
                        "Tryb administratora został odblokowany dla tego elementu. Zmiany zostaną wykonane przez KIO admin:// i zweryfikowane po zapisie.",
                        "Administrator mode is unlocked for this item. Changes will be performed through KIO admin:// and verified after saving."));
            });

        // --------------------------------------------------------------
        // Buttons / write-back with verification
        // --------------------------------------------------------------
        auto *buttons =
            new QDialogButtonBox(
                QDialogButtonBox::Ok
                    | QDialogButtonBox::Apply
                    | QDialogButtonBox::Cancel,
                &dialog);
        outer->addWidget(buttons);

        auto applyChanges = [&]() -> bool {
            bool permissionWriteVerifiedThisApply = false;

            if (canEditPermissions
                && !readOnlyFileSystem) {
                const int requestedMode =
                    modeFromPermissionBoxes();
                const bool recursive =
                    isDir
                    && recursivePermissions->isChecked();

                KJob *chmodJob = nullptr;
                auto &recoveryGate = BatchRenameRecoveryGate::instance();
                recoveryGate.refresh();
                if (recoveryGate.mutationsBlocked()) {
                    QMessageBox::warning(&dialog, trLocal("Właściwości", "Properties"),
                                         recoveryGate.message());
                    return false;
                }

                if (recursive) {
                    KFileItem rootItem;
                    QString statError;

                    if (!readKioFileItem(
                            workingUrl,
                            &rootItem,
                            &statError)) {
                        QMessageBox::warning(
                            &dialog,
                            trLocal(
                                "Nie udało się rozpocząć zmiany uprawnień",
                                "Could not start permission change"),
                            isPolish()
                                ? QStringLiteral(
                                    "Nie udało się odczytać informacji potrzebnych do zmiany rekurencyjnej:\n%1")
                                    .arg(statError)
                                : QStringLiteral(
                                    "Could not read the information required for a recursive permission change:\n%1")
                                    .arg(statError));
                        return false;
                    }

                    KFileItemList items;
                    items.push_back(rootItem);

                    chmodJob = KIO::chmod(
                        items,
                        requestedMode,
                        0777,
                        QString(),
                        QString(),
                        true,
                        KIO::DefaultFlags);
                } else {
                    chmodJob = KIO::chmod(
                        workingUrl,
                        requestedMode);
                }

                if (!chmodJob->exec()) {
                    if (url.isLocalFile()
                        && !adminUnlocked) {
                        adminUnlockButton->show();
                    }

                    QString failureText =
                        isPolish()
                            ? QStringLiteral(
                                "System odmówił zmiany uprawnień:\n%1\n\nJeżeli element należy do innego użytkownika, użyj „Odblokuj jako administrator”.")
                                .arg(chmodJob->errorString())
                            : QStringLiteral(
                                "The system refused the permission change:\n%1\n\nIf the item belongs to another user, use “Unlock as administrator”.")
                                .arg(chmodJob->errorString());

                    if (permissionBehaviorMayDependOnMount) {
                        failureText +=
                            isPolish()
                                ? QStringLiteral(
                                    "\n\nTen wolumin jest zgłaszany jako „%1”. Jeżeli nawet administrator nie może zmienić praw, sprawdź sposób montowania woluminu. Dla ntfs-3g prawdziwe chmod wymaga konfiguracji obsługującej uprawnienia, np. permissions wraz z poprawnym UserMapping.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem)
                                : QStringLiteral(
                                    "\n\nThis volume is reported as “%1”. If even administrator mode cannot change the permissions, check the volume mount configuration. With ntfs-3g, real chmod requires a permissions-capable configuration, for example permissions with a valid UserMapping.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem);
                    }

                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Nie udało się zmienić uprawnień",
                            "Could not change permissions"),
                        failureText);
                    return false;
                }

                int verifiedMode = -1;
                QString verifyError;

                if (!readKioPermissions(
                        workingUrl,
                        &verifiedMode,
                        nullptr,
                        nullptr,
                        &verifyError)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Nie można zweryfikować uprawnień",
                            "Could not verify permissions"),
                        isPolish()
                            ? QStringLiteral(
                                "Polecenie zmiany zostało wykonane, ale nie udało się ponownie odczytać praw z dysku:\n%1")
                                .arg(
                                    verifyError)
                            : QStringLiteral(
                                "The change operation completed, but the permissions could not be read back from disk:\n%1")
                                .arg(
                                    verifyError));
                    return false;
                }

                setPermissionBoxesFromMode(
                    verifiedMode);
                currentMode = verifiedMode;

                if ((verifiedMode & 0777)
                    != (requestedMode & 0777)) {
                    if (url.isLocalFile()
                        && !adminUnlocked) {
                        adminUnlockButton->show();
                    }

                    QString mismatchText =
                        isPolish()
                            ? QStringLiteral(
                                "System zaakceptował operację, ale po ponownym odczycie prawa nadal mają wartość %1 zamiast %2.")
                                .arg(
                                    QString::number(
                                        verifiedMode & 0777,
                                        8),
                                    QString::number(
                                        requestedMode & 0777,
                                        8))
                            : QStringLiteral(
                                "The operation completed, but after reading the file back its mode is still %1 instead of %2.")
                                .arg(
                                    QString::number(
                                        verifiedMode & 0777,
                                        8),
                                    QString::number(
                                        requestedMode & 0777,
                                        8));

                    if (permissionBehaviorMayDependOnMount) {
                        mismatchText +=
                            isPolish()
                                ? QStringLiteral(
                                    "\n\nNa tym woluminie (%1) sterownik lub opcje montowania nie zachowały żądanej zmiany. Dla ntfs-3g sprawdź, czy wolumin jest montowany z permissions i poprawnym .NTFS-3G/UserMapping; konfiguracje oparte na stałych uid/gid/umask mogą ignorować chmod.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem)
                                : QStringLiteral(
                                    "\n\nOn this volume (%1), the driver or mount options did not preserve the requested change. With ntfs-3g, check that the volume is mounted with permissions and a valid .NTFS-3G/UserMapping; configurations based on fixed uid/gid/umask can ignore chmod.")
                                    .arg(
                                        fileSystem.isEmpty()
                                            ? QStringLiteral("?")
                                            : fileSystem);
                    } else {
                        mismatchText +=
                            trLocal(
                                "\n\nMoże to oznaczać brak wystarczających uprawnień do tego elementu.",
                                "\n\nThis can indicate insufficient privileges for this item.");
                    }

                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Uprawnienia nie zostały zmienione",
                            "Permissions did not change"),
                        mismatchText);
                    return false;
                }

                permissionWriteVerifiedThisApply = true;
            }

            const QString requestedName =
                nameEdit->text().trimmed();

            if ((workingUrl.isLocalFile()
                 || isAdminUrl(workingUrl))
                && !requestedName.isEmpty()
                && requestedName != workingName) {
                if (!validNewName(
                        requestedName)) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Właściwości",
                            "Properties"),
                        trLocal(
                            "Podana nazwa jest nieprawidłowa.",
                            "The requested name is invalid."));
                    return false;
                }

                const QUrl newUrl =
                    siblingUrlWithName(
                        workingUrl,
                        requestedName);

                auto &recoveryGate = BatchRenameRecoveryGate::instance();
                recoveryGate.refresh();
                if (recoveryGate.mutationsBlocked()) {
                    QMessageBox::warning(&dialog, trLocal("Właściwości", "Properties"),
                                         recoveryGate.message());
                    return false;
                }

                KIO::CopyJob *renameJob =
                    KIO::moveAs(
                        workingUrl,
                        newUrl,
                        KIO::HideProgressInfo);

                if (recordCopyJob) {
                    recordCopyJob(renameJob);
                }

                if (!renameJob->exec()) {
                    QMessageBox::warning(
                        &dialog,
                        trLocal(
                            "Właściwości",
                            "Properties"),
                        isPolish()
                            ? QStringLiteral(
                                "Nie udało się zmienić nazwy:\n%1")
                                .arg(
                                    renameJob->errorString())
                            : QStringLiteral(
                                "Could not rename the item:\n%1")
                                .arg(
                                    renameJob->errorString()));
                    return false;
                }

                workingUrl = newUrl;
                workingName = requestedName;
                headerName->setText(
                    requestedName);
            }

            const bool recursiveApplied =
                isDir
                && recursivePermissions->isChecked();

            QString successText;
            if (recursiveApplied) {
                successText =
                    adminUnlocked
                        ? trLocal(
                            "Zastosowano zmianę rekurencyjnie przez KIO admin://. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied through KIO admin://. The top-level folder permissions were read back and verified.")
                        : trLocal(
                            "Zastosowano zmianę rekurencyjnie. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied. The top-level folder permissions were read back and verified.");
            } else {
                successText =
                    adminUnlocked
                        ? trLocal(
                            "Zapisano i zweryfikowano zmiany przez KIO admin://.",
                            "Changes were saved and verified through KIO admin://.")
                        : trLocal(
                            "Zapisano i zweryfikowano zmiany.",
                            "Changes were saved and verified.");
            }

            if (permissionWriteVerifiedThisApply
                && permissionBehaviorMayDependOnMount) {
                successText +=
                    isPolish()
                        ? QStringLiteral(
                            "\n\nZmiana została faktycznie potwierdzona po ponownym odczycie na woluminie „%1”. Dla tej konfiguracji edycja uprawnień działa i nie jest blokowana ze względu na sam typ systemu plików.")
                            .arg(
                                fileSystem.isEmpty()
                                    ? QStringLiteral("?")
                                    : fileSystem)
                        : QStringLiteral(
                            "\n\nThe change was confirmed by reading the mode back on the “%1” volume. Permission editing works with this configuration and is not blocked based on the filesystem type alone.")
                            .arg(
                                fileSystem.isEmpty()
                                    ? QStringLiteral("?")
                                    : fileSystem);
            }

            permissionsInfo->setText(successText);

            if (showStatus) {
                showStatus(
                    trLocal(
                        "Zapisano zmiany we właściwościach.",
                        "Property changes saved."),
                    3500);
            }

            if (refreshView) {
                refreshView();
            }
            return true;
        };

        QObject::connect(
            buttons->button(
                QDialogButtonBox::Apply),
            &QPushButton::clicked,
            &dialog,
            [&] {
                applyChanges();
            });

        QObject::connect(
            buttons,
            &QDialogButtonBox::accepted,
            &dialog,
            [&] {
                if (applyChanges()) {
                    dialog.accept();
                }
            });

        QObject::connect(
            buttons,
            &QDialogButtonBox::rejected,
            &dialog,
            &QDialog::reject);

        dialog.exec();
    }

private:
    static QString filesystemTypeForUrl(const QUrl &url)
    {
        const QString path =
            localPathForFileOrAdmin(url);

        if (path.isEmpty()) {
            return {};
        }

        QStorageInfo storage(path);
        if (!storage.isValid()) {
            return {};
        }

        return QString::fromLatin1(
            storage.fileSystemType()).toLower();
    }

    static bool filesystemIsReadOnly(const QUrl &url)
    {
        const QString path =
            localPathForFileOrAdmin(url);

        if (path.isEmpty()) {
            return false;
        }

        QStorageInfo storage(path);
        return storage.isValid()
            && storage.isReadOnly();
    }

    static bool filesystemMayUseMountControlledPermissions(
        const QString &fileSystem)
    {
        const QString fs =
            fileSystem.trimmed().toLower();

        return fs == QStringLiteral("ntfs")
            || fs == QStringLiteral("ntfs3")
            || fs == QStringLiteral("fuseblk")
            || fs == QStringLiteral("exfat")
            || fs == QStringLiteral("vfat")
            || fs == QStringLiteral("fat")
            || fs == QStringLiteral("fat32")
            || fs == QStringLiteral("msdos");
    }

    static bool readKioPermissions(
        const QUrl &url,
        int *mode,
        QString *owner = nullptr,
        QString *group = nullptr,
        QString *error = nullptr)
    {
        if (!url.isValid()) {
            if (error) {
                *error = QStringLiteral("Invalid URL");
            }
            return false;
        }

        KIO::StatJob *job =
            KIO::stat(
                url,
                KIO::HideProgressInfo);

        if (!job->exec()) {
            if (error) {
                *error = job->errorString();
            }
            return false;
        }

        const KIO::UDSEntry entry =
            job->statResult();

        const qlonglong access =
            entry.numberValue(
                KIO::UDSEntry::UDS_ACCESS,
                -1);

        if (access < 0) {
            if (error) {
                *error = QStringLiteral(
                    "Permission bits were not returned by the KIO worker.");
            }
            return false;
        }

        if (mode) {
            *mode = static_cast<int>(access) & 0777;
        }

        if (owner) {
            *owner =
                entry.stringValue(
                    KIO::UDSEntry::UDS_USER);
        }

        if (group) {
            *group =
                entry.stringValue(
                    KIO::UDSEntry::UDS_GROUP);
        }

        return true;
    }

    static bool readKioFileItem(
        const QUrl &url,
        KFileItem *item,
        QString *error = nullptr)
    {
        if (!url.isValid() || !item) {
            if (error) {
                *error = QStringLiteral("Invalid URL");
            }
            return false;
        }

        KIO::StatJob *job =
            KIO::stat(
                url,
                KIO::HideProgressInfo);

        if (!job->exec()) {
            if (error) {
                *error = job->errorString();
            }
            return false;
        }

        *item = KFileItem(
            job->statResult(),
            url,
            true,
            false);
        return true;
    }

    static bool localEntryOwnedByCurrentUser(
        const QUrl &url)
    {
        if (!url.isLocalFile()) {
            return false;
        }

    #ifdef Q_OS_UNIX
        const QFileInfo info(url.toLocalFile());
        return info.exists()
            && (geteuid() == 0
                || info.ownerId()
                    == static_cast<uint>(geteuid()));
    #else
        return QFileInfo(url.toLocalFile()).isWritable();
    #endif
    }

};
