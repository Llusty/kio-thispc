/* SPDX-License-Identifier: MIT */
#pragma once
#include "aggregatepropertiesscan.h"
#include "aggregatepropertiesformat.h"
#include "aggregatepermissionswidget.h"
#include "browsercommon.h"
#include "propertiesshell.h"
#include <QFormLayout>
#include "soliddevicemonitor.h"
#include <QDialog>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QTimer>
#include <mutex>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

class AggregatePropertiesDialog final : public QDialog {
public:
    explicit AggregatePropertiesDialog(const QList<QUrl> &urls, QWidget *parent = nullptr)
        : QDialog(parent), cancel(std::make_shared<std::atomic_bool>(false)) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(trLocal("Właściwości — wiele pozycji", "Properties — multiple items"));
        setObjectName(QStringLiteral("aggregatePropertiesDialog"));
        PropertiesShell shell(this, trLocal("Zaznaczone pozycje: %1", "Selected items: %1").arg(aggregateCount(urls.size())), themedIcon(QStringLiteral("document-multiple")));
        auto *scroll = PropertiesShell::scrollArea(shell.tabs);
        auto *general = new QWidget(scroll);
        scroll->setWidget(general);
        shell.tabs->addTab(scroll, trLocal("Ogólne", "General"));
        auto *layout = new QVBoxLayout(general);
        layout->setSizeConstraint(QLayout::SetMinimumSize);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        layout->addLayout(form);
        const auto row = [general, form](const QString &title, const QString &name, const QString &value) {
            auto *label = new WrappingValueLabel(value, general);
            label->setObjectName(name);
            label->setTextFormat(Qt::PlainText);
            form->addRow(title, label);
            return label;
        };
        row(trLocal("Wybrane elementy:", "Selected items:"), QStringLiteral("aggregateSelection"), aggregateCount(urls.size()));
        summary = row(trLocal("Status:", "Status:"), QStringLiteral("aggregateSummary"), trLocal("Obliczanie…", "Calculating…"));
        files = row(trLocal("Pliki:", "Files:"), QStringLiteral("aggregateFiles"), QStringLiteral("0"));
        folders = row(trLocal("Foldery:", "Folders:"), QStringLiteral("aggregateFolders"), QStringLiteral("0"));
        links = row(trLocal("Dowiązania symboliczne:", "Symbolic links:"), QStringLiteral("aggregateLinks"), QStringLiteral("0"));
        bytes = row(trLocal("Rozmiar logiczny:", "Logical size:"), QStringLiteral("aggregateBytes"), QStringLiteral("0 B"));
        skipped = row(trLocal("Pominięte / nieczytelne:", "Skipped / unreadable:"), QStringLiteral("aggregateSkipped"), QStringLiteral("0"));
        unsupported = row(trLocal("Nieobsługiwane:", "Unsupported:"), QStringLiteral("aggregateUnsupported"), QStringLiteral("0"));
        invalidated = row(trLocal("Zmienione / brakujące:", "Changed / missing:"), QStringLiteral("aggregateInvalidated"), QStringLiteral("0"));
        disk = row(trLocal("Rozmiar na dysku:", "Size on disk:"), QStringLiteral("aggregateDiskSize"), trLocal("Obliczanie…", "Calculating…"));
        disk->setToolTip(trLocal("Alokacja zwykłych plików raportowana przez system plików (st_blocks × 512). Hardlinki liczone raz. Bez katalogów i dowiązań symbolicznych. Nie oznacza unikalnego fizycznego zajęcia: reflink/CoW i kompresja Btrfs oraz sposób raportowania NTFS mogą wpływać na wynik.", "Regular-file allocation reported by the filesystem (st_blocks × 512). Hardlinks counted once. Excludes directories and symlinks. Not unique physical usage: reflink/CoW, Btrfs compression and NTFS reporting may affect the result."));
        auto *policy = new WrappingValueLabel(trLocal("Tylko pliki lokalne. Uwzględnia pozycje ukryte. Dowiązania symboliczne są liczone osobno, bez podążania za nimi. Pomija granice urządzeń. Rozmiar logiczny liczy każde dowiązanie twarde; nakładające się zaznaczenia liczy raz. Alokacja plików jest raportowana przez system plików, hardlinki liczone raz; nie oznacza unikalnego fizycznego zajęcia danych współdzielonych lub skompresowanych.", "Local files only. Hidden entries included. Symbolic links are counted separately and never followed. Mount boundaries are skipped. Logical size counts each hardlink entry; overlapping selections are counted once. File allocation is reported by the filesystem, hardlinks counted once; it is not unique physical usage of shared or compressed data."), general);
        layout->addWidget(policy);
        progress = new QProgressBar(general);
        progress->setRange(0, 0);
        layout->addWidget(progress);
        layout->addStretch();
        permissions = new AggregatePermissionsWidget(urls, shell.tabs);
        shell.tabs->addTab(permissions, trLocal("Uprawnienia", "Permissions"));
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok | QDialogButtonBox::Apply, this);
        permissions->stateChanged = [this, buttons] { buttons->button(QDialogButtonBox::Apply)->setEnabled(permissions->canApply()); };
        buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { closeAfterApply = false; permissions->requestApply(); });
        shell.outer->addWidget(buttons);
        connect(buttons->button(QDialogButtonBox::Cancel), &QAbstractButton::clicked, this, [this, buttons] {
            permissions->stop();
            cancel->store(true);
            if (scanFinished) { close(); return; }
            summary->setText(trLocal("Anulowanie…", "Cancelling…"));
            buttons->button(QDialogButtonBox::Cancel)->hide();
        });
        connect(buttons->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this] {
            if (permissions->isBusy()) return;
            if (permissions->edit().changeMask) { QPointer<AggregatePropertiesDialog> alive(this); closeAfterApply = true; permissions->requestApply(); if (alive && !permissions->isBusy()) closeAfterApply = false; }
            else close();
        });
        struct ProgressState { std::mutex mutex; AggregateSummary summary; };
        auto state = std::make_shared<ProgressState>();
        auto *timer = new QTimer(this);
        timer->setInterval(150);
        connect(timer, &QTimer::timeout, this, [this, state] {
            if (changedSelection || cancel->load()) return;
            std::lock_guard lock(state->mutex);
            updateCounts(state->summary);
        });
        timer->start();
        const auto snapshot = AggregateSnapshot::capture(urls);
        liveSnapshot = snapshot;
        auto *changes = new QFileSystemWatcher(this);
        QStringList paths;
        for (const auto &root : snapshot.roots) if (root.valid) {
            paths << root.url.toLocalFile() << QFileInfo(root.url.toLocalFile()).absolutePath();
        }
        paths.removeDuplicates();
        changes->addPaths(paths);
        const auto changed = [this] {
            if (permissions->isBusy()) return;
            for (const auto &root : liveSnapshot.roots) if (root.valid) {
                struct stat now {};
                if (::lstat(root.path.constData(), &now) || now.st_dev != root.identity.st_dev
                    || now.st_ino != root.identity.st_ino || now.st_ctim.tv_sec != root.identity.st_ctim.tv_sec
                    || now.st_ctim.tv_nsec != root.identity.st_ctim.tv_nsec) {
                    changedSelection = true;
                    permissions->invalidate();
                    cancel->store(true);
                    summary->setText(trLocal("Wynik częściowy — zaznaczona pozycja zmieniła się lub zniknęła. Otwórz Właściwości ponownie.", "Partial results — selected item changed or disappeared. Reopen Properties to calculate again."));
                    break;
                }
            }
        };
        auto *devices = new SolidDeviceMonitor(this);
        connect(devices, &SolidDeviceMonitor::devicesChanged, this, changed);
        connect(changes, &QFileSystemWatcher::fileChanged, this, changed);
        connect(changes, &QFileSystemWatcher::directoryChanged, this, changed);
        auto *watcher = new QFutureWatcher<AggregateSummary>(this);
        connect(watcher, &QFutureWatcher<AggregateSummary>::finished, this, [this, watcher, buttons, timer] {
            timer->stop();
            scanFinished = true;
            const auto result = watcher->result();
            progress->hide();
            buttons->button(QDialogButtonBox::Cancel)->setVisible(!cancel->load() && !result.cancelled);
            updateCounts(result);
            permissions->setReady(!cancel->load() && !result.cancelled && !changedSelection);
            if (changedSelection) return;
            summary->setText(result.cancelled ? trLocal("Anulowano — wynik częściowy", "Cancelled — partial results")
                : result.limited ? trLocal("Osiągnięto limit skanowania — wynik częściowy", "Scan limit reached — partial results")
                : result.partial() ? trLocal("Wynik częściowy", "Partial results") : trLocal("Zakończono", "Complete"));
        });
        permissions->applied = [this](bool success) {
            for (auto &root : liveSnapshot.roots) {
                struct stat now {};
                if (!root.valid) continue;
                if (::lstat(root.path.constData(), &now) || now.st_dev != root.identity.st_dev
                    || now.st_ino != root.identity.st_ino || now.st_size != root.identity.st_size
                    || now.st_blocks != root.identity.st_blocks
                    || now.st_mtim.tv_sec != root.identity.st_mtim.tv_sec
                    || now.st_mtim.tv_nsec != root.identity.st_mtim.tv_nsec) {
                    changedSelection = true;
                    permissions->invalidate();
                    summary->setText(trLocal("Wynik częściowy — pozycja lub jej zawartość zmieniła się. Otwórz Właściwości ponownie.", "Partial results — entry or its contents changed. Reopen Properties."));
                } else root.identity = now; // chmod changes mode/ctime, not size/mtime.
            }
            if (closeAfterApply && success && !changedSelection) close();
            closeAfterApply = false;
        };
        watcher->setFuture(QtConcurrent::run([snapshot, token = cancel, state] {
            return AggregatePropertiesScan::run(snapshot, token, 1000000, [state](const AggregateSummary &summary) {
                std::lock_guard lock(state->mutex);
                state->summary = summary;
            });
        }));
    }
    ~AggregatePropertiesDialog() override { cancel->store(true); }
protected:
    void closeEvent(QCloseEvent *event) override { permissions->stop(); cancel->store(true); QDialog::closeEvent(event); }
    void reject() override { permissions->stop(); cancel->store(true); QDialog::reject(); }
private:
    AggregateSnapshot liveSnapshot;
    AggregatePermissionsWidget *permissions = nullptr;
    bool scanFinished = false, closeAfterApply = false;
    void updateCounts(const AggregateSummary &result) {
        files->setText(aggregateCount(result.files));
        folders->setText(aggregateCount(result.folders));
        links->setText(aggregateCount(result.links));
        bytes->setText(aggregateSize(result.bytes));
        QString allocation = !result.allocatedFiles && (result.partial() || result.allocationUnavailable)
            ? trLocal("Niedostępny", "Unavailable") : aggregateSize(result.allocatedBytes);
        if (result.partial() || result.allocationUnavailable)
            allocation += trLocal(" — wynik częściowy", " — partial results");
        disk->setText(allocation);
        disk->setToolTip(disk->toolTip().section(QLatin1Char('\n'), 0, 0)
            + QStringLiteral("\n") + trLocal("Brak danych alokacji: %1", "Allocation unavailable: %1").arg(aggregateCount(result.allocationUnavailable)));
        skipped->setText(aggregateCount(result.skipped));
        unsupported->setText(aggregateCount(result.unsupported));
        invalidated->setText(aggregateCount(result.invalidated));
    }
    QLabel *disk, *files, *folders, *links, *bytes, *skipped, *unsupported, *invalidated;
    AggregatePropertiesScan::Cancel cancel;
    bool changedSelection = false;
    QLabel *summary;
    QProgressBar *progress;
};
