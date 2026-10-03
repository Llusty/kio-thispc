/*
 * Properties dialog and permission editing support.
 *
 * Extracted during the 0.21.0 architecture refactor.
 * Updated in 0.37.0 Stage 1 to use PropertiesDataProvider for data modeling,
 * capability detection, and truthful size/inode/timestamp inspection.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "batchrenamerecovery.h"
#include "browsercommon.h"
#include "propertiesdata.h"
#include "propertiesdataprovider.h"
#include "drivepropertiesdata.h"
#include "drivepropertiesprovider.h"
#include "acleditorwidget.h"
#include "checksumwidget.h"
#include "metadatawidget.h"

#include <KFileItem>
#include <KIO/ChmodJob>
#include <KIO/CopyJob>
#include <KIO/StatJob>
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
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStorageInfo>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <memory>
#include <sys/stat.h>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

class WrappingValueLabel : public QLabel
{
public:
    explicit WrappingValueLabel(const QString &text = QString(), QWidget *parent = nullptr)
        : QLabel(text.isEmpty() ? QStringLiteral("—") : text, parent)
    {
        setWordWrap(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(40, fontMetrics().height());
    }

    void setValue(const QString &text)
    {
        setText(text.isEmpty() ? QStringLiteral("—") : text);
    }
};

// Modeless Properties window. While an Apply is running (synchronous KIO
// jobs spin a nested event loop) close requests are deferred so the dialog is
// never destroyed underneath its own lambdas.
class PropertiesWindow : public QDialog
{
public:
    using QDialog::QDialog;

    bool isBusy() const { return m_busy; }

    void setBusy(bool busy)
    {
        m_busy = busy;
        if (!busy && m_pending) {
            m_pending = false;
            QDialog::done(m_pendingResult);
        }
    }

    void done(int result) override
    {
        if (m_busy) {
            m_pending = true;
            m_pendingResult = result;
            return;
        }
        QDialog::done(result);
    }

private:
    bool m_busy = false;
    bool m_pending = false;
    int m_pendingResult = 0;
};

// Small registry of open Properties dialogs (file, folder and drive). It lets
// an owning main window close its dialogs deterministically and lets tests
// observe lifetime. Entries are QPointers, so destroyed dialogs drop out.
class PropertiesLifecycle
{
public:
    static PropertiesLifecycle &instance()
    {
        static PropertiesLifecycle registry;
        return registry;
    }

    void add(QDialog *dialog)
    {
        prune();
        if (dialog) m_open.append(dialog);
    }

    int openCount()
    {
        prune();
        return m_open.size();
    }

    QList<QDialog *> openDialogs()
    {
        prune();
        QList<QDialog *> result;
        for (const auto &d : m_open) result.append(d.data());
        return result;
    }

    // Close every dialog owned by `owner`. Dialogs busy with Apply finish
    // their job first and close afterwards.
    void closeAllFor(QWidget *owner)
    {
        const auto open = openDialogs();
        for (QDialog *dialog : open) {
            if (dialog && dialog->parentWidget() == owner) dialog->close();
        }
    }

private:
    void prune()
    {
        m_open.removeIf([](const QPointer<QDialog> &d) { return d.isNull(); });
    }

    QList<QPointer<QDialog>> m_open;
};

// Stable (device, inode) identity of a local target, so Apply never writes to
// a different file that replaced the original path.
struct PropertiesIdentity
{
    static bool read(const QString &path, quint64 *dev, quint64 *ino)
    {
        struct stat info {};
        if (path.isEmpty()
            || ::lstat(QFile::encodeName(path).constData(), &info) != 0) {
            return false;
        }
        *dev = static_cast<quint64>(info.st_dev);
        *ino = static_cast<quint64>(info.st_ino);
        return true;
    }
};

class PropertiesDialog final
{
public:
    using EnsureAdminProtocol = std::function<bool()>;
    using RecordCopyJob = std::function<void(KIO::CopyJob *)>;
    using ShowStatus = std::function<void(const QString &, int)>;
    using RefreshView = std::function<void()>;

    static QDialog *show(
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

        auto *dialog = new PropertiesWindow(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(false);
        dialog->setObjectName(QStringLiteral("propertiesDialog"));
        dialog->setMinimumSize(480, 460);
        QPointer<PropertiesWindow> alive(dialog);
        dialog->setWindowTitle(
            trLocal("Właściwości — ", "Properties — ") + name);
        dialog->resize(630, 680);

        // Mutable state shared by the Apply/unlock lambdas. It lives on the
        // heap so the dialog stays valid after show() returns (modeless).
        struct State {
            QUrl workingUrl;
            QString workingName;
            bool canEditPermissions = false;
            bool adminUnlocked = false;
            int currentMode = -1;
            QString permissionOwner;
            QString permissionGroup;
            bool haveIdentity = false;
            quint64 dev = 0;
            quint64 ino = 0;
        };
        auto st = std::make_shared<State>();
        st->workingUrl = url;
        st->workingName = name;

        const QString physicalPath =
            localPathForFileOrAdmin(url);
        if (url.isLocalFile() || isAdminUrl(url)) {
            st->haveIdentity = PropertiesIdentity::read(
                physicalPath, &st->dev, &st->ino);
        }
        const QString fileSystem =
            filesystemTypeForUrl(url);
        const bool permissionBehaviorMayDependOnMount =
            filesystemMayUseMountControlledPermissions(
                fileSystem);
        const bool readOnlyFileSystem =
            filesystemIsReadOnly(url);

        auto *outer = new QVBoxLayout(dialog);

        auto *header = new QHBoxLayout;
        auto *iconLabel = new QLabel(dialog);
        QIcon icon = themedIcon(
            fileItem.iconName().isEmpty()
                ? (isDir
                    ? QStringLiteral("folder")
                    : QStringLiteral("text-x-generic"))
                : fileItem.iconName());
        iconLabel->setPixmap(icon.pixmap(64, 64));
        iconLabel->setFixedSize(72, 72);
        iconLabel->setAlignment(Qt::AlignCenter);

        auto *headerName = new QLabel(name, dialog);
        QFont headerFont = headerName->font();
        headerFont.setPointSize(
            headerFont.pointSize() + 3);
        headerFont.setBold(true);
        headerName->setFont(headerFont);
        headerName->setWordWrap(true);

        header->addWidget(iconLabel);
        header->addWidget(headerName, 1);
        outer->addLayout(header);

        auto *tabs = new QTabWidget(dialog);
        outer->addWidget(tabs, 1);

        auto makeValueLabel =
            [dialog](const QString &value = QString()) {
            return new WrappingValueLabel(value, dialog);
        };

        // --------------------------------------------------------------
        // General (Presentation Layer fed by PropertiesDataProvider)
        // --------------------------------------------------------------
        auto *general = new QWidget(tabs);
        auto *generalForm =
            new QFormLayout(general);
        generalForm->setFieldGrowthPolicy(
            QFormLayout::AllNonFixedFieldsGrow);

        auto *nameEdit =
            new QLineEdit(name, general);
        nameEdit->setObjectName(QStringLiteral("propertiesName"));
        nameEdit->setReadOnly(
            !(url.isLocalFile()
              || isAdminUrl(url)));
        generalForm->addRow(
            trLocal("Nazwa:", "Name:"),
            nameEdit);

        auto *typeLabel = makeValueLabel(typeText);
        generalForm->addRow(trLocal("Typ:", "Type:"), typeLabel);

        auto *mimeLabel = makeValueLabel();
        generalForm->addRow(QStringLiteral("MIME:"), mimeLabel);

        auto *symlinkIndicatorRowLabel = new QLabel(trLocal("Dowiązanie:", "Symbolic link:"), general);
        auto *symlinkIndicatorLabel = makeValueLabel();
        generalForm->addRow(symlinkIndicatorRowLabel, symlinkIndicatorLabel);
        symlinkIndicatorRowLabel->hide();
        symlinkIndicatorLabel->hide();

        auto *symlinkTargetRowLabel = new QLabel(trLocal("Cel dowiązania:", "Link target:"), general);
        auto *symlinkTargetLabel = makeValueLabel();
        generalForm->addRow(symlinkTargetRowLabel, symlinkTargetLabel);
        symlinkTargetRowLabel->hide();
        symlinkTargetLabel->hide();

        auto *sizeLabel = makeValueLabel(sizeText);
        generalForm->addRow(trLocal("Rozmiar:", "Size:"), sizeLabel);

        auto *diskSizeLabel = makeValueLabel();
        generalForm->addRow(trLocal("Rozmiar na dysku:", "Size on disk:"), diskSizeLabel);

        auto *fsLabel = makeValueLabel(fileSystem.isEmpty() ? QString() : fileSystem);
        generalForm->addRow(trLocal("System plików:", "Filesystem:"), fsLabel);

        auto *inodeLabel = makeValueLabel();
        generalForm->addRow(trLocal("Inode:", "Inode:"), inodeLabel);

        auto *ownerLabel = makeValueLabel();
        generalForm->addRow(trLocal("Właściciel:", "Owner:"), ownerLabel);

        auto *groupLabel = makeValueLabel();
        generalForm->addRow(trLocal("Grupa:", "Group:"), groupLabel);

        auto *modifiedLabel = makeValueLabel(modifiedText);
        generalForm->addRow(trLocal("Zmodyfikowano:", "Modified:"), modifiedLabel);

        auto *createdLabel = makeValueLabel();
        generalForm->addRow(trLocal("Utworzono:", "Created:"), createdLabel);

        auto *accessedLabel = makeValueLabel();
        generalForm->addRow(trLocal("Ostatni dostęp:", "Accessed:"), accessedLabel);

        auto *metadataChangedLabel = makeValueLabel();
        generalForm->addRow(trLocal("Zmiana metadanych:", "Metadata changed:"), metadataChangedLabel);

        auto *locationLabel = makeValueLabel();
        generalForm->addRow(trLocal("Lokalizacja:", "Location:"), locationLabel);

        auto *addressLabel = makeValueLabel(urlForDisplay(url));
        generalForm->addRow(trLocal("Adres:", "Address:"), addressLabel);

        auto updateGeneral = [=](const PropertiesData &data) {
            if (!data.friendlyType.isEmpty()) {
                typeLabel->setText(data.friendlyType);
            }
            if (!data.mimeType.isEmpty()) {
                mimeLabel->setText(data.mimeType);
            }

            if (data.isSymLink) {
                symlinkIndicatorRowLabel->show();
                symlinkIndicatorLabel->show();
                symlinkIndicatorLabel->setText(data.isBrokenSymLink
                    ? trLocal("Tak (przerwane)", "Yes (broken)")
                    : trLocal("Tak", "Yes"));

                symlinkTargetRowLabel->show();
                symlinkTargetLabel->show();
                symlinkTargetLabel->setText(data.symLinkTarget.isEmpty()
                    ? QStringLiteral("—")
                    : data.symLinkTarget);
            } else {
                symlinkIndicatorRowLabel->hide();
                symlinkIndicatorLabel->hide();
                symlinkTargetRowLabel->hide();
                symlinkTargetLabel->hide();
            }

            if (data.isDir) {
                sizeLabel->setText(!sizeText.isEmpty() ? sizeText : QStringLiteral("—"));
                diskSizeLabel->setText(trLocal("Niedostępne", "Unavailable"));
            } else if (data.hasLogicalSize) {
                sizeLabel->setText(data.formattedLogicalSize());
                diskSizeLabel->setText(data.formattedAllocatedSize());
            } else {
                sizeLabel->setText(!sizeText.isEmpty() ? sizeText : QStringLiteral("—"));
                diskSizeLabel->setText(trLocal("Niedostępne", "Unavailable"));
            }

            fsLabel->setText(data.fileSystem.isEmpty()
                ? trLocal("Niedostępne", "Unavailable")
                : data.fileSystem);

            inodeLabel->setText(data.hasInode
                ? QString::number(data.inode)
                : trLocal("Niedostępne", "Unavailable"));

            ownerLabel->setText(data.hasOwner
                ? data.owner
                : trLocal("Niedostępne", "Unavailable"));

            groupLabel->setText(data.hasGroup
                ? data.group
                : trLocal("Niedostępne", "Unavailable"));

            modifiedLabel->setText(data.hasModifiedTime
                ? PropertiesData::formatDateTime(data.modifiedTime)
                : (!modifiedText.isEmpty() ? modifiedText : QStringLiteral("—")));

            createdLabel->setText(data.hasBirthTime
                ? PropertiesData::formatDateTime(data.birthTime)
                : trLocal("Niedostępne", "Unavailable"));

            accessedLabel->setText(data.hasAccessTime
                ? PropertiesData::formatDateTime(data.accessTime)
                : QStringLiteral("—"));

            metadataChangedLabel->setText(data.hasMetadataChangeTime
                ? PropertiesData::formatDateTime(data.metadataChangeTime)
                : trLocal("Niedostępne", "Unavailable"));

            locationLabel->setText(data.location.isEmpty()
                ? QStringLiteral("—")
                : data.location);

            addressLabel->setText(data.displayAddress.isEmpty()
                ? urlForDisplay(data.url)
                : data.displayAddress);
        };

        auto *provider = new PropertiesDataProvider(dialog);
        QObject::connect(provider, &PropertiesDataProvider::dataReady, dialog, updateGeneral);
        QObject::connect(provider, &PropertiesDataProvider::dataUpdated, dialog, updateGeneral);
        provider->load(st->workingUrl, name, isDir, typeText, sizeText, modifiedText);

        tabs->addTab(
            general,
            themedIcon(
                QStringLiteral(
                    "document-properties")),
            trLocal("Ogólne", "General"));

        auto *checksumPage = new ChecksumWidget(st->workingUrl, tabs);
        tabs->addTab(
            checksumPage,
            themedIcon(QStringLiteral("document-encrypt")),
            trLocal("Sumy kontrolne", "Checksums"));

        auto *metadataPage = new MetadataWidget(st->workingUrl, tabs);
        const int metadataTab = tabs->addTab(
            metadataPage,
            themedIcon(QStringLiteral("documentinfo")),
            trLocal("Metadane", "Metadata"));
        QObject::connect(tabs, &QTabWidget::currentChanged, metadataPage,
                         [metadataPage, metadataTab](int index) {
            if (index == metadataTab) metadataPage->activate();
        });

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
            [=](bool enabled) {
            for (QCheckBox *box :
                 permissionBoxes) {
                box->setEnabled(enabled);
            }
            recursivePermissions->setEnabled(
                enabled && isDir);
        };

        auto setPermissionBoxesFromMode =
            [=](int mode) {
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
            [=]() {
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

        st->adminUnlocked = isAdminUrl(st->workingUrl);
        QString permissionReadError;

        const bool canReadPermissions =
            readKioPermissions(
                st->workingUrl,
                &st->currentMode,
                &st->permissionOwner,
                &st->permissionGroup,
                &permissionReadError);

        if (canReadPermissions) {
            setPermissionBoxesFromMode(
                st->currentMode);
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

            if (ordinaryLocal && !st->adminUnlocked) {
                adminUnlockButton->setVisible(true);
            }
        } else if (st->adminUnlocked) {
            st->canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Uprawnienia są edytowane przez KIO admin://. Zapis może wywołać systemowe okno autoryzacji PolicyKit.",
                    "Permissions are being edited through KIO admin://. Saving may invoke the system PolicyKit authentication dialog."));
        } else if (ownedByCurrentUser) {
            st->canEditPermissions = true;
            setPermissionBoxesEnabled(true);
            permissionsInfo->setText(
                trLocal(
                    "Jesteś właścicielem tego elementu. Zmiany zostaną zapisane po użyciu „Zastosuj” lub „OK” i zweryfikowane ponownym odczytem z dysku.",
                    "You own this item. Changes are written after pressing Apply or OK and verified by reading the permissions back from disk."));
        } else {
            st->canEditPermissions = false;
            setPermissionBoxesEnabled(false);

            permissionsInfo->setText(
                isPolish()
                    ? QStringLiteral(
                        "Nie jesteś właścicielem tego elementu%1. Zmiana uprawnień wymaga autoryzacji administratora.")
                        .arg(
                            st->permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (właściciel: %1)")
                                    .arg(st->permissionOwner))
                    : QStringLiteral(
                        "You do not own this item%1. Changing permissions requires administrator authorization.")
                        .arg(
                            st->permissionOwner.isEmpty()
                                ? QString()
                                : QStringLiteral(
                                    " (owner: %1)")
                                    .arg(st->permissionOwner)));

            adminUnlockButton->setVisible(true);
        }

        if (ordinaryLocal
            && !st->adminUnlocked
            && !readOnlyFileSystem
            && !ownedByCurrentUser) {
            adminUnlockButton->setVisible(true);
        }

        AclData aclData;
        if (ordinaryLocal) {
            const QFileInfo aclInfo(physicalPath);
            aclData = AclProvider::load(physicalPath, true, aclInfo.isSymLink(), isDir, readOnlyFileSystem);
        } else if (isAdminUrl(st->workingUrl)) {
            aclData.capability = AclCapability::PermissionDenied;
            aclData.errorMessage = trLocal("Edycja ACL przez admin:// nie należy do tego etapu.", "ACL editing through admin:// is not part of this stage.");
        } else {
            aclData = AclProvider::unavailableForRemote();
        }
        auto *aclEditor = new AclEditorWidget(aclData, permissionsPage);
        permissionsLayout->addWidget(aclEditor);
        aclEditor->setDirtyChangedCallback([=](bool dirty) {
            if (dirty) recursivePermissions->setChecked(false);
            recursivePermissions->setEnabled(st->canEditPermissions && isDir && !dirty);
        });

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
            dialog,
            [=] {
                if (!ensureAdminProtocol || !ensureAdminProtocol()) {
                    return;
                }

                const QString localPath =
                    localPathForFileOrAdmin(
                        st->workingUrl);

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
                        dialog,
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

                st->workingUrl = elevatedUrl;
                st->adminUnlocked = true;
                st->canEditPermissions = true;
                st->currentMode = elevatedMode;
                st->permissionOwner =
                    elevatedOwner;
                st->permissionGroup =
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
                dialog);
        outer->addWidget(buttons);

        auto applyChanges = [=]() -> bool {
            if (!alive || alive->isBusy()) return false;

            // The target may have vanished or been replaced while this
            // modeless dialog was open. Never write to a different item.
            if (st->haveIdentity) {
                quint64 dev = 0, ino = 0;
                if (!PropertiesIdentity::read(
                        localPathForFileOrAdmin(st->workingUrl), &dev, &ino)
                    || dev != st->dev || ino != st->ino) {
                    st->canEditPermissions = false;
                    setPermissionBoxesEnabled(false);
                    nameEdit->setEnabled(false);
                    permissionsInfo->setText(trLocal(
                        "Element został usunięty, przeniesiony lub zastąpiony. Zmiany nie zostały zapisane.",
                        "The item was removed, moved or replaced. No changes were saved."));
                    QMessageBox::warning(dialog, trLocal("Właściwości", "Properties"),
                        trLocal("Element nie jest już dostępny pod tą ścieżką.",
                                "The item is no longer available at this path."));
                    return false;
                }
            }

            struct BusyScope {
                QPointer<PropertiesWindow> window;
                QPointer<QDialogButtonBox> box;
                ~BusyScope()
                {
                    if (box) box->setEnabled(true);
                    if (window) window->setBusy(false);
                }
            } busyScope{alive, buttons};
            alive->setBusy(true);
            buttons->setEnabled(false);

            bool permissionWriteVerifiedThisApply = false;
            bool aclWriteThisApply = false;

            if (aclEditor->isDirty()) {
                if (recursivePermissions->isChecked()) {
                    QMessageBox::warning(dialog, trLocal("Uprawnienia ACL", "ACL permissions"), trLocal("ACL i rekurencyjna zmiana chmod nie mogą być zapisane razem.", "ACL and recursive chmod cannot be saved together."));
                    return false;
                }
                QString aclError;
                const int requestedMode = modeFromPermissionBoxes();
                if (!aclEditor->prepareForWrite(requestedMode, &aclError)) {
                    QMessageBox::warning(dialog, trLocal("Nieprawidłowa ACL", "Invalid ACL"), aclError);
                    return false;
                }
                const AclController::Result result = aclEditor->write();
                if (!result.success) {
                    QMessageBox::warning(dialog, trLocal("Nie udało się zapisać ACL", "Could not save ACL"), result.errorMessage);
                    return false;
                }
                int verifiedMode = -1;
                QString verifyError;
                if (!readKioPermissions(st->workingUrl, &verifiedMode, nullptr, nullptr, &verifyError)) {
                    QMessageBox::warning(dialog, trLocal("Nie można zweryfikować uprawnień", "Could not verify permissions"), verifyError);
                    return false;
                }
                st->currentMode = verifiedMode;
                setPermissionBoxesFromMode(verifiedMode);
                permissionWriteVerifiedThisApply = true;
                aclWriteThisApply = true;
            }

            if (st->canEditPermissions
                && !readOnlyFileSystem
                && !aclWriteThisApply) {
                const int requestedMode =
                    modeFromPermissionBoxes();
                const bool recursive =
                    isDir
                    && recursivePermissions->isChecked();

                KJob *chmodJob = nullptr;
                auto &recoveryGate = BatchRenameRecoveryGate::instance();
                recoveryGate.refresh();
                if (recoveryGate.mutationsBlocked()) {
                    QMessageBox::warning(dialog, trLocal("Właściwości", "Properties"),
                                         recoveryGate.message());
                    return false;
                }

                if (recursive) {
                    KFileItem rootItem;
                    QString statError;

                    if (!readKioFileItem(
                            st->workingUrl,
                            &rootItem,
                            &statError)) {
                        QMessageBox::warning(
                            dialog,
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
                        st->workingUrl,
                        requestedMode);
                }

                const bool chmodOk = chmodJob->exec();
                if (!alive) return false;
                if (!chmodOk) {
                    if (url.isLocalFile()
                        && !st->adminUnlocked) {
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
                        dialog,
                        trLocal(
                            "Nie udało się zmienić uprawnień",
                            "Could not change permissions"),
                        failureText);
                    return false;
                }

                int verifiedMode = -1;
                QString verifyError;

                if (!readKioPermissions(
                        st->workingUrl,
                        &verifiedMode,
                        nullptr,
                        nullptr,
                        &verifyError)) {
                    QMessageBox::warning(
                        dialog,
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
                st->currentMode = verifiedMode;

                if ((verifiedMode & 0777)
                    != (requestedMode & 0777)) {
                    if (url.isLocalFile()
                        && !st->adminUnlocked) {
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
                        dialog,
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

            if ((st->workingUrl.isLocalFile()
                 || isAdminUrl(st->workingUrl))
                && !requestedName.isEmpty()
                && requestedName != st->workingName) {
                if (!validNewName(
                        requestedName)) {
                    QMessageBox::warning(
                        dialog,
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
                        st->workingUrl,
                        requestedName);

                auto &recoveryGate = BatchRenameRecoveryGate::instance();
                recoveryGate.refresh();
                if (recoveryGate.mutationsBlocked()) {
                    QMessageBox::warning(dialog, trLocal("Właściwości", "Properties"),
                                         recoveryGate.message());
                    return false;
                }

                const QString targetLocalPath = localPathForFileOrAdmin(newUrl);
                if (!targetLocalPath.isEmpty() && QFile::exists(targetLocalPath)) {
                    QMessageBox::warning(
                        dialog,
                        trLocal("Właściwości", "Properties"),
                        trLocal("Element o tej nazwie już istnieje.",
                                "An item with that name already exists."));
                    return false;
                }

                KIO::CopyJob *renameJob =
                    KIO::moveAs(
                        st->workingUrl,
                        newUrl,
                        KIO::HideProgressInfo);
                renameJob->setUiDelegate(nullptr);

                if (recordCopyJob) {
                    recordCopyJob(renameJob);
                }

                const bool renameOk = renameJob->exec();
                if (!alive) return false;
                if (!renameOk) {
                    QMessageBox::warning(
                        dialog,
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

                st->workingUrl = newUrl;
                st->workingName = requestedName;
                checksumPage->setUrl(st->workingUrl);
                metadataPage->setUrl(st->workingUrl);
                headerName->setText(
                    requestedName);
            }

            const bool recursiveApplied =
                isDir
                && recursivePermissions->isChecked();

            QString successText;
            if (recursiveApplied) {
                successText =
                    st->adminUnlocked
                        ? trLocal(
                            "Zastosowano zmianę rekurencyjnie przez KIO admin://. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied through KIO admin://. The top-level folder permissions were read back and verified.")
                        : trLocal(
                            "Zastosowano zmianę rekurencyjnie. Uprawnienia folderu głównego zostały ponownie odczytane i zweryfikowane.",
                            "The recursive change was applied. The top-level folder permissions were read back and verified.");
            } else {
                successText =
                    st->adminUnlocked
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
            dialog,
            [=] {
                applyChanges();
            });

        QObject::connect(
            buttons,
            &QDialogButtonBox::accepted,
            dialog,
            [=] {
                if (applyChanges()) {
                    dialog->accept();
                }
            });

        QObject::connect(
            buttons,
            &QDialogButtonBox::rejected,
            dialog,
            &QDialog::reject);

        PropertiesLifecycle::instance().add(dialog);
        dialog->show();
        return dialog;
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

public:
    static QDialog *showForDrive(
        QWidget *parent,
        const DriveInfo &drive,
        const QString &fakeMountInfo = {},
        const DrivePropertiesData *snapshot = nullptr)
    {
        auto *dialog = new QDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(false);
        dialog->setObjectName(QStringLiteral("drivePropertiesDialog"));
        dialog->setWindowTitle(
            trLocal("Właściwości dysku — ", "Drive Properties — ") + drive.name);
        dialog->resize(580, 640);

        auto *outer = new QVBoxLayout(dialog);

        // Header
        auto *header = new QHBoxLayout;
        auto *iconLabel = new QLabel(dialog);
        QString iconName = drive.iconName;
        if (iconName.isEmpty()) {
            iconName = drive.isRemovable
                ? QStringLiteral("drive-removable-media")
                : QStringLiteral("drive-harddisk");
        }
        iconLabel->setPixmap(themedIcon(iconName).pixmap(56, 56));
        iconLabel->setFixedSize(64, 64);
        iconLabel->setAlignment(Qt::AlignCenter);

        auto *headerTextLayout = new QVBoxLayout;
        headerTextLayout->setSpacing(2);
        auto *headerName = new QLabel(drive.name, dialog);
        headerName->setObjectName(QStringLiteral("driveHeaderName"));
        QFont headerFont = headerName->font();
        headerFont.setPointSize(headerFont.pointSize() + 3);
        headerFont.setBold(true);
        headerName->setFont(headerFont);
        headerName->setWordWrap(true);

        auto *headerSubtitle = new QLabel(
            drive.isMounted ? drive.mountPoint : trLocal("Niezamontowany", "Not mounted"),
            dialog);
        headerSubtitle->setObjectName(QStringLiteral("driveHeaderSubtitle"));
        headerSubtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);

        headerTextLayout->addWidget(headerName);
        headerTextLayout->addWidget(headerSubtitle);

        header->addWidget(iconLabel);
        header->addLayout(headerTextLayout, 1);
        outer->addLayout(header);

        // Disconnected device warning banner (hidden by default)
        auto *warningBanner = new QLabel(dialog);
        warningBanner->setObjectName(QStringLiteral("deviceRemovedBanner"));
        warningBanner->setText(trLocal("⚠️ To urządzenie zostało odłączone od systemu.",
                                       "⚠️ This device has been disconnected from the system."));
        warningBanner->setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(220, 50, 50, 0.15); border: 1px solid rgba(220, 50, 50, 0.5); padding: 6px; border-radius: 4px; font-weight: bold; }"));
        warningBanner->setWordWrap(true);
        warningBanner->hide();
        outer->addWidget(warningBanner);

        auto *tabs = new QTabWidget(dialog);
        outer->addWidget(tabs, 1);

        auto makeValueLabel = [dialog](const QString &initial = QString()) {
            return new WrappingValueLabel(initial, dialog);
        };

        // Tab: General / Ogólne
        auto *generalScroll = new QScrollArea(tabs);
        generalScroll->setFrameShape(QFrame::NoFrame);
        generalScroll->setWidgetResizable(true);
        generalScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto *generalWidget = new QWidget(generalScroll);
        auto *generalLayout = new QVBoxLayout(generalWidget);
        generalLayout->setSpacing(10);
        generalLayout->setContentsMargins(4, 4, 4, 4);

        // Group 1: Identity & Hardware
        auto *idGroup = new QGroupBox(trLocal("Identyfikacja i sprzęt", "Identity & Hardware"), generalWidget);
        auto *idForm = new QFormLayout(idGroup);
        idForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto *labelValue = makeValueLabel(drive.name);
        labelValue->setObjectName(QStringLiteral("driveLabelValue"));
        idForm->addRow(trLocal("Etykieta:", "Label:"), labelValue);

        auto *deviceNodeValue = makeValueLabel();
        deviceNodeValue->setObjectName(QStringLiteral("driveDeviceNodeValue"));
        idForm->addRow(trLocal("Węzeł urządzenia:", "Device node:"), deviceNodeValue);

        auto *parentDriveValue = makeValueLabel();
        parentDriveValue->setObjectName(QStringLiteral("driveParentDriveValue"));
        idForm->addRow(trLocal("Dysk nadrzędny:", "Parent drive:"), parentDriveValue);

        auto *driveModelValue = makeValueLabel();
        driveModelValue->setObjectName(QStringLiteral("driveModelValue"));
        idForm->addRow(trLocal("Model napędu:", "Drive model:"), driveModelValue);

        auto *driveVendorValue = makeValueLabel();
        driveVendorValue->setObjectName(QStringLiteral("driveVendorValue"));
        idForm->addRow(trLocal("Producent:", "Vendor:"), driveVendorValue);

        auto *driveBusValue = makeValueLabel();
        driveBusValue->setObjectName(QStringLiteral("driveBusValue"));
        idForm->addRow(trLocal("Magistrala:", "Bus:"), driveBusValue);

        auto *uuidValue = makeValueLabel();
        uuidValue->setObjectName(QStringLiteral("driveUuidValue"));
        idForm->addRow(trLocal("UUID:", "UUID:"), uuidValue);

        generalLayout->addWidget(idGroup);

        // Group 2: Filesystem & Mount
        auto *fsGroup = new QGroupBox(trLocal("System plików i montowanie", "Filesystem & Mount"), generalWidget);
        auto *fsForm = new QFormLayout(fsGroup);
        fsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto *fsTypeValue = makeValueLabel(drive.fileSystem);
        fsTypeValue->setObjectName(QStringLiteral("driveFsTypeValue"));
        fsForm->addRow(trLocal("System plików:", "Filesystem:"), fsTypeValue);

        auto *mountedValue = makeValueLabel(
            drive.isMounted ? trLocal("Zamontowany", "Mounted") : trLocal("Niezamontowany", "Not mounted"));
        mountedValue->setObjectName(QStringLiteral("driveMountedValue"));
        fsForm->addRow(trLocal("Stan:", "Status:"), mountedValue);

        auto *accessModeValue = makeValueLabel();
        accessModeValue->setObjectName(QStringLiteral("driveAccessModeValue"));
        fsForm->addRow(trLocal("Tryb dostępu:", "Access mode:"), accessModeValue);

        auto *removableValue = makeValueLabel(
            drive.isRemovable ? trLocal("Tak", "Yes") : trLocal("Nie", "No"));
        removableValue->setObjectName(QStringLiteral("driveRemovableValue"));
        fsForm->addRow(trLocal("Urządzenie wymienne:", "Removable device:"), removableValue);

        auto *hotplugValue = makeValueLabel();
        hotplugValue->setObjectName(QStringLiteral("driveHotplugValue"));
        fsForm->addRow(trLocal("Obsługa hot-plug:", "Hot-pluggable:"), hotplugValue);

        auto *mountPointValue = makeValueLabel(
            drive.isMounted ? drive.mountPoint : trLocal("—", "—"));
        mountPointValue->setObjectName(QStringLiteral("driveMountPointValue"));
        fsForm->addRow(trLocal("Punkt montowania:", "Mount point:"), mountPointValue);

        auto *mountOptionsValue = makeValueLabel();
        mountOptionsValue->setObjectName(QStringLiteral("driveMountOptionsValue"));
        fsForm->addRow(trLocal("Opcje montowania:", "Mount options:"), mountOptionsValue);

        generalLayout->addWidget(fsGroup);

        // Group 3: Capacity & Space
        auto *spaceGroup = new QGroupBox(trLocal("Pojemność i przestrzeń", "Capacity & Space"), generalWidget);
        auto *spaceForm = new QFormLayout(spaceGroup);
        spaceForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        auto *totalSpaceValue = makeValueLabel(drive.capacityText);
        totalSpaceValue->setObjectName(QStringLiteral("driveTotalSpaceValue"));
        spaceForm->addRow(trLocal("Pojemność:", "Capacity:"), totalSpaceValue);

        auto *usedSpaceValue = makeValueLabel(drive.usedText);
        usedSpaceValue->setObjectName(QStringLiteral("driveUsedSpaceValue"));
        spaceForm->addRow(trLocal("Zajęte miejsce:", "Used space:"), usedSpaceValue);

        auto *freeSpaceValue = makeValueLabel(drive.freeText);
        freeSpaceValue->setObjectName(QStringLiteral("driveFreeSpaceValue"));
        spaceForm->addRow(trLocal("Wolne miejsce:", "Free space:"), freeSpaceValue);

        auto *usageBar = new QProgressBar(spaceGroup);
        usageBar->setObjectName(QStringLiteral("driveUsageBar"));
        usageBar->setRange(0, 100);
        usageBar->setValue(drive.usedPercent);
        usageBar->setTextVisible(true);
        usageBar->setFormat(QStringLiteral("%p%"));
        spaceForm->addRow(trLocal("Zajętość:", "Usage:"), usageBar);

        generalLayout->addWidget(spaceGroup);
        generalLayout->addStretch(1);

        generalScroll->setWidget(generalWidget);
        tabs->addTab(generalScroll, trLocal("Ogólne", "General"));

        // Close button box
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::accept);
        outer->addWidget(buttons);

        // Provider wiring
        auto *provider = new DrivePropertiesProvider(dialog);

        auto updateUiWithData = [=](const DrivePropertiesData &data) {
            if (!data.name.isEmpty()) {
                labelValue->setText(data.name);
            }
            if (!data.deviceNode.isEmpty()) {
                deviceNodeValue->setText(data.deviceNode);
            }
            if (!data.parentDriveNode.isEmpty()) {
                parentDriveValue->setText(data.parentDriveNode);
            }
            if (!data.driveModel.isEmpty()) {
                driveModelValue->setText(data.driveModel);
            }
            if (!data.driveVendor.isEmpty()) {
                driveVendorValue->setText(data.driveVendor);
            }
            if (!data.driveBus.isEmpty()) {
                driveBusValue->setText(data.driveBus);
            }
            if (!data.uuid.isEmpty()) {
                uuidValue->setText(data.uuid);
            }

            fsTypeValue->setText(data.formattedFileSystem());
            mountedValue->setText(data.isMounted
                ? trLocal("Zamontowany", "Mounted")
                : trLocal("Niezamontowany", "Not mounted"));

            if (data.isMounted) {
                accessModeValue->setText(data.isReadOnly
                    ? trLocal("Tylko do odczytu (ro)", "Read-only (ro)")
                    : trLocal("Odczyt i zapis (rw)", "Read/Write (rw)"));
                mountPointValue->setText(data.mountPoint.isEmpty() ? QStringLiteral("—") : data.mountPoint);
                mountOptionsValue->setText(data.formattedMountOptions());
                headerSubtitle->setText(data.mountPoint.isEmpty() ? trLocal("Zamontowany", "Mounted") : data.mountPoint);
            } else {
                accessModeValue->setText(QStringLiteral("—"));
                mountPointValue->setText(QStringLiteral("—"));
                mountOptionsValue->setText(QStringLiteral("—"));
                headerSubtitle->setText(trLocal("Niezamontowany", "Not mounted"));
            }

            removableValue->setText(data.isRemovable
                ? trLocal("Tak", "Yes")
                : trLocal("Nie", "No"));
            hotplugValue->setText(data.isHotpluggable
                ? trLocal("Tak", "Yes")
                : trLocal("Nie", "No"));

            totalSpaceValue->setText(data.formattedTotal());
            usedSpaceValue->setText(data.formattedUsed());
            freeSpaceValue->setText(data.formattedFree());

            if (data.isMounted && data.hasTotalBytes()) {
                usageBar->setEnabled(true);
                usageBar->setValue(data.usedPercent);
                usageBar->setFormat(QStringLiteral("%p%"));
            } else {
                usageBar->setEnabled(false);
                usageBar->setValue(0);
                usageBar->setFormat(trLocal("Niedostępne", "Unavailable"));
            }

            if (data.isDeviceMissing) {
                warningBanner->show();
            }
        };

        QObject::connect(provider, &DrivePropertiesProvider::dataReady, dialog, updateUiWithData);
        QObject::connect(provider, &DrivePropertiesProvider::deviceRemoved, dialog, [=] {
            warningBanner->show();
            headerSubtitle->setText(trLocal("Odłączony", "Disconnected"));
            mountedValue->setText(trLocal("Odłączony", "Disconnected"));
            usageBar->setEnabled(false);
        });

        if (snapshot) {
            provider->loadSnapshot(*snapshot);
        } else {
            provider->load(drive, fakeMountInfo);
        }

        PropertiesLifecycle::instance().add(dialog);
        dialog->show();
        return dialog;
    }
};
