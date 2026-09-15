/*
 * File-operation dialogs and KIO dispatch, independent of the active pane.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"
#include "localfilecopyjob.h"
#include "localfilemovejob.h"
#include "undocontroller.h"
#include <KIO/JobUiDelegateFactory>
#include <KIO/MkdirJob>
#include <KIO/RenameDialog>
#include <KIO/StoredTransferJob>
#include <KJobUiDelegate>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <functional>
#include <utility>

class FileActions final : public QObject
{
public:
    using WatchOperation = std::function<void(KJob *, const QString &, bool, const QString &)>;

    FileActions(QWidget *parentWidget, UndoController *undoController, WatchOperation watchOperation)
        : QObject(parentWidget)
        , m_parentWidget(parentWidget)
        , m_undoController(undoController)
        , m_watchOperation(std::move(watchOperation))
    {
    }

    // Arguments are snapshots captured by the initiating pane before any
    // modal dialog can change focus. This class never queries the active pane.
    void pasteClipboardInto(const QUrl &destination)
    {
        if (!destination.isValid()) {
            return;
        }

        const QMimeData *mime =
            QApplication::clipboard()->mimeData();

        if (!mime || !mime->hasUrls()) {
            return;
        }

        const QList<QUrl> urls = mime->urls();

        if (urls.isEmpty()) {
            return;
        }

        const bool cut =
            mime->data(
                QStringLiteral("application/x-kde-cutselection"))
                == QByteArrayLiteral("1");

        if (startNativeSingleFileTransfer(
                urls, destination, cut, cut,
                cut
                    ? trLocal("Przenoszenie zakończone", "Move completed")
                    : trLocal("Kopiowanie zakończone", "Copy completed"),
                cut
                    ? trLocal("Przenoszenie", "Moving")
                    : trLocal("Kopiowanie", "Copying"))) {
            return;
        }

        KIO::CopyJob *job =
            cut
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            cut
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            cut,
            cut
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

    void copySelectionToDirectory(
        const QList<QUrl> &urls,
        const QString &directory,
        const QString &successMessage)
    {
        if (urls.isEmpty()
            || directory.isEmpty()) {
            return;
        }

        QDir target(directory);
        if (!target.exists()
            && !QDir().mkpath(directory)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Wyślij do", "Send to"),
                trLocal(
                    "Nie udało się utworzyć katalogu docelowego.",
                    "Could not create the destination directory."));
            return;
        }

        if (startNativeSingleFileTransfer(
                urls, QUrl::fromLocalFile(directory), false, false,
                successMessage, trLocal("Kopiowanie", "Copying"))) {
            return;
        }

        auto *job =
            KIO::copy(
                urls,
                QUrl::fromLocalFile(directory),
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            successMessage,
            false,
            trLocal("Kopiowanie", "Copying"));
    }

    void createNewFile(
        const QUrl &directory,
        const QString &suggestedName,
        const QByteArray &contents)
    {
        if (!directory.isValid()) {
            return;
        }

        const QString name = requestNewFileName(suggestedName);
        if (name.isEmpty()) return;

        const QUrl destination =
            childUrlWithName(directory, name);

        KIO::StoredTransferJob *job =
            KIO::storedPut(
                contents,
                destination,
                -1,
                KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        if (m_undoController) {
            m_undoController->recordPutJob(
                destination,
                job);
        }

        watchFileOperation(
            job,
            trLocal("Utworzono plik", "File created"),
            false,
            trLocal("Tworzenie pliku", "Creating file"));
    }

    void createFromTemplate(QUrl directory, QUrl source)
    {
        if (!directory.isValid() || !source.isLocalFile()) return;

        const QString name = requestNewFileName(source.fileName());
        if (name.isEmpty()) return;

        // Recheck after the modal dialog: a stale entry must not turn into
        // a recursive folder copy or create a symbolic link as a document.
        const QFileInfo info(source.toLocalFile());
        if (info.isSymLink() || (info.exists() && !info.isFile())) {
            QMessageBox::warning(m_parentWidget,
                trLocal("Szablony", "Templates"),
                trLocal("Szablon nie jest zwykłym plikiem.", "The template is not a regular file."));
            return;
        }

        const QUrl destination = childUrlWithName(directory, name);
        KIO::CopyJob *job = KIO::copyAs(source, destination, KIO::HideProgressInfo);
        configureInteractiveCopyJob(job);
        if (m_undoController) m_undoController->recordCopyJob(job);
        watchFileOperation(job,
            trLocal("Utworzono plik", "File created"), false,
            trLocal("Tworzenie pliku", "Creating file"));
    }

    void createNewFolder(const QUrl &directory)
    {
        if (!directory.isValid()) {
            return;
        }

        bool ok = false;

        const QString name =
            QInputDialog::getText(
                m_parentWidget,
                trLocal("Nowy folder", "New folder"),
                trLocal("Nazwa folderu:", "Folder name:"),
                QLineEdit::Normal,
                trLocal("Nowy folder", "New folder"),
                &ok)
                .trimmed();

        if (!ok) {
            return;
        }

        if (!validNewName(name)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa folderu jest pusta albo zawiera niedozwolony znak „/”.",
                    "The folder name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            childUrlWithName(directory, name);

        KIO::MkdirJob *job =
            KIO::mkdir(destination);

        job->setUiDelegate(nullptr);
        if (m_undoController) {
            m_undoController->recordMkdirJob(
                destination,
                job);
        }

        watchFileOperation(
            job,
            trLocal("Utworzono folder", "Folder created"),
            false,
            trLocal("Tworzenie folderu", "Creating folder"));
    }

    void renameSelected(const QList<QUrl> &urls, QString oldName)
    {

        if (urls.size() != 1) {
            return;
        }

        const QUrl source = urls.first();


        if (oldName.isEmpty()) {
            oldName =
                QFileInfo(source.path()).fileName();
        }

        bool ok = false;

        const QString newName =
            QInputDialog::getText(
                m_parentWidget,
                trLocal("Zmień nazwę", "Rename"),
                trLocal("Nowa nazwa:", "New name:"),
                QLineEdit::Normal,
                oldName,
                &ok)
                .trimmed();

        if (!ok || newName == oldName) {
            return;
        }

        if (!validNewName(newName)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa jest pusta albo zawiera niedozwolony znak „/”.",
                    "The name is empty or contains the invalid “/” character."));
            return;
        }

        const QUrl destination =
            siblingUrlWithName(source, newName);

        KIO::CopyJob *job =
            KIO::moveAs(
                source,
                destination,
                KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            trLocal("Zmieniono nazwę", "Renamed"),
            false,
            trLocal("Zmiana nazwy", "Renaming"));
    }

    void trashSelected(const QList<QUrl> &urls)
    {

        if (urls.isEmpty()) {
            return;
        }

        for (const QUrl &url : urls) {
            if (!url.isLocalFile()) {
                QMessageBox::information(
                    m_parentWidget,
                    trLocal("Kosz", "Trash"),
                    trLocal(
                        "W tej wersji usuwanie do Kosza jest dostępne tylko dla lokalnych plików i folderów.",
                        "In this version, moving to Trash is available only for local files and folders."));
                return;
            }
        }

        const QString question =
            isPolish()
                ? QStringLiteral(
                    "Przenieść zaznaczone elementy do Kosza?\n\nLiczba elementów: %1")
                    .arg(urls.size())
                : QStringLiteral(
                    "Move the selected items to Trash?\n\nItems: %1")
                    .arg(urls.size());

        if (QMessageBox::question(
                m_parentWidget,
                trLocal("Przenieś do Kosza", "Move to Trash"),
                question,
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }

        KIO::CopyJob *job =
            KIO::trash(
                urls,
                KIO::HideProgressInfo);

        job->setUiDelegate(nullptr);
        if (m_undoController) {
            m_undoController->recordTrashJob(
                urls,
                job);
        }

        watchFileOperation(
            job,
            trLocal(
                "Przeniesiono do Kosza",
                "Moved to Trash"),
            false,
            trLocal("Przenoszenie do Kosza", "Moving to Trash"));
    }

    void transfer(const QList<QUrl> &urls, const QUrl &destination, Qt::DropAction action)
    {
        if (urls.isEmpty() || !destination.isValid()) return;
        const bool move = action == Qt::MoveAction;
        if (startNativeSingleFileTransfer(
                urls, destination, move, false,
                move
                    ? trLocal("Przenoszenie zakończone", "Move completed")
                    : trLocal("Kopiowanie zakończone", "Copy completed"),
                move
                    ? trLocal("Przenoszenie", "Moving")
                    : trLocal("Kopiowanie", "Copying"))) {
            return;
        }
        KIO::CopyJob *job =
            action == Qt::MoveAction
                ? KIO::move(
                    urls,
                    destination,
                    KIO::HideProgressInfo)
                : KIO::copy(
                    urls,
                    destination,
                    KIO::HideProgressInfo);

        configureInteractiveCopyJob(job);
        if (m_undoController) {
            m_undoController->recordCopyJob(job);
        }

        watchFileOperation(
            job,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie zakończone", "Move completed")
                : trLocal("Kopiowanie zakończone", "Copy completed"),
            false,
            action == Qt::MoveAction
                ? trLocal("Przenoszenie", "Moving")
                : trLocal("Kopiowanie", "Copying"));
    }

private:
    QString requestNewFileName(const QString &suggestedName)
    {
        bool ok = false;
        const QString initial = suggestedName.isEmpty()
            ? trLocal("Nowy plik", "New file") : suggestedName;
        const QString name = QInputDialog::getText(
            m_parentWidget,
            trLocal("Nowy plik", "New file"),
            trLocal("Nazwa pliku:", "File name:"),
            QLineEdit::Normal, initial, &ok).trimmed();
        if (!ok) return {};
        if (!validNewName(name)) {
            QMessageBox::warning(
                m_parentWidget,
                trLocal("Nieprawidłowa nazwa", "Invalid name"),
                trLocal(
                    "Nazwa pliku jest pusta albo zawiera niedozwolony znak „/”.",
                    "The file name is empty or contains the invalid “/” character."));
            return {};
        }
        return name;
    }

    bool startNativeSingleFileTransfer(
        const QList<QUrl> &sources,
        const QUrl &destinationDirectory,
        bool move,
        bool clearClipboard,
        const QString &successMessage,
        const QString &title)
    {
        if (sources.isEmpty() || !destinationDirectory.isLocalFile()) {
            return false;
        }
        for (const QUrl &source : sources) if (!source.isLocalFile()) return false;
        const QFileInfo sourceInfo(sources.first().toLocalFile());
        const QFileInfo destinationInfo(destinationDirectory.toLocalFile());
        if (!destinationInfo.exists() || !destinationInfo.isDir()) return false;
        if (sources.size() > 1 || sourceInfo.isDir() || sourceInfo.isSymLink()) {
            auto *job = new LocalTransferJob(sources, destinationDirectory, move, this);
            job->setFallbackFactory([this, sources, destinationDirectory, move] {
                auto *fallback = move
                    ? KIO::move(sources, destinationDirectory, KIO::HideProgressInfo)
                    : KIO::copy(sources, destinationDirectory, KIO::HideProgressInfo);
                configureInteractiveCopyJob(fallback);
                if (m_undoController) m_undoController->recordCopyJob(fallback);
                return fallback;
            });
            if (m_undoController) m_undoController->recordNativeTransfer(job, move);
            watchFileOperation(job, successMessage, clearClipboard, title);
            job->start();
            return true;
        }
        if (!sourceInfo.exists() || !sourceInfo.isFile()
            || sourceInfo.isSymLink()
            || !destinationInfo.exists() || !destinationInfo.isDir()) {
            return false;
        }
        QUrl destination = childUrlWithName(
            destinationDirectory, sourceInfo.fileName());
        bool overwrite = false;
        LocalFileIdentity originalDestination;
        while (true) {
            const QFileInfo targetInfo(destination.toLocalFile());
            if (!targetInfo.exists() && !targetInfo.isSymLink()) break;
            if (!targetInfo.isFile() || targetInfo.isSymLink()
                || LocalFileIdentity::read(sourceInfo.absoluteFilePath())
                    .sameFile(LocalFileIdentity::read(destination.toLocalFile()))) {
                // Keep KIO's handling of type conflicts and self-copy, also
                // when a renamed destination introduces such a conflict.
                auto *job = move
                    ? KIO::moveAs(sources.first(), destination, KIO::HideProgressInfo)
                    : KIO::copyAs(sources.first(), destination, KIO::HideProgressInfo);
                configureInteractiveCopyJob(job);
                if (m_undoController) m_undoController->recordCopyJob(job);
                watchFileOperation(job, successMessage, clearClipboard, title);
                return true;
            }
            originalDestination = LocalFileIdentity::read(destination.toLocalFile());
            KIO::RenameDialog dialog(
                m_parentWidget, title, sources.first(), destination,
                KIO::RenameDialog_Overwrite | KIO::RenameDialog_Skip
                    | KIO::RenameDialog_MultipleItems,
                sourceInfo.size(), targetInfo.size(),
                sourceInfo.birthTime(), targetInfo.birthTime(),
                sourceInfo.lastModified(), targetInfo.lastModified());
            const int decision = dialog.exec();
            if (decision == KIO::Result_Overwrite || decision == KIO::Result_OverwriteAll
                || (decision == KIO::Result_OverwriteWhenOlder
                    && sourceInfo.lastModified() > targetInfo.lastModified())) {
                overwrite = true;
                break;
            }
            if (decision == KIO::Result_Rename || decision == KIO::Result_AutoRename) {
                destination = decision == KIO::Result_Rename
                    ? dialog.newDestUrl() : dialog.autoDestUrl();
                if (destination.isLocalFile() && !destination.fileName().isEmpty()) continue;
            }
            // Skip and Cancel do not dispatch a job, clear cut data, or
            // add an Undo command. The existing files remain untouched.
            return true;
        }

        KIO::Job *job = move
            ? static_cast<KIO::Job *>(new LocalFileMoveJob(
                  sources.first(), destination, this,
                  LocalFileMoveStrategy::Automatic, 256 * 1024, 0, {}, overwrite,
                  originalDestination))
            : static_cast<KIO::Job *>(new LocalFileCopyJob(
                  sources.first(), destination, this, 256 * 1024, 0, overwrite,
                  false, originalDestination));
        if (m_undoController) {
            m_undoController->recordNativeTransfer(
                job, move);
        }
        watchFileOperation(job, successMessage, clearClipboard, title);
        job->start();
        return true;
    }

    void configureInteractiveCopyJob(KIO::CopyJob *job)
    {
        if (!job) {
            return;
        }

        // KIO's WidgetsAskUserActionHandler is attached through the default
        // widgets UI delegate.  We keep automatic error/warning handling
        // disabled because watchFileOperation() already owns our error UI;
        // conflict/skip/rename questions remain fully interactive.
        if (KJobUiDelegate *delegate =
                KIO::createDefaultJobUiDelegate(
                    KJobUiDelegate::AutoHandlingDisabled,
                    m_parentWidget)) {
            job->setUiDelegate(delegate);
        }
    }

    void watchFileOperation(KJob *job, const QString &message, bool clearClipboard, const QString &title)
    {
        m_watchOperation(job, message, clearClipboard, title);
    }

    QWidget *m_parentWidget = nullptr;
    UndoController *m_undoController = nullptr;
    WatchOperation m_watchOperation;
};
