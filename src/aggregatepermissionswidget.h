/* SPDX-License-Identifier: MIT */
#pragma once
#include "aggregatepermissionswriter.h"
#include "browsercommon.h"
#include <QPushButton>
#include "batchrenamerecovery.h"
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <array>

class AggregatePermissionsWidget final : public QWidget {
public:
    explicit AggregatePermissionsWidget(const QList<QUrl> &urls, QWidget *parent = nullptr)
        : QWidget(parent), model(AggregatePermissionsModel::capture(urls)), token(std::make_shared<std::atomic_bool>(false)) {
        setObjectName(QStringLiteral("aggregatePermissions"));
        auto *layout = new QVBoxLayout(this);
        auto *policy = new QLabel(trLocal(
            "Tylko bezpośrednio zaznaczone pozycje, bez rekurencji. Wykonanie oznacza przechodzenie dla folderów. Stan bieżący i żądana zmiana są osobne. Pozostałe bity rwx i 07000 są zachowywane. chmod może zmienić maskę ACL i efektywne prawa wpisów nazwanych. Nie edytuje wpisów ACL ani domyślnych ACL. Brak atomowości całego zestawu.",
            "Selected entries only; no recursion. Execute means traverse for folders. Current state and requested change are separate. Other rwx bits and 07000 are preserved. chmod may change the ACL mask and named entries' effective rights. ACL entries and default ACLs are not edited. The batch is not atomic."), this);
        policy->setWordWrap(true); layout->addWidget(policy);
        auto *group = new QGroupBox(trLocal("Uprawnienia POSIX", "POSIX permissions"), this);
        auto *grid = new QGridLayout(group);
        const QStringList columns{trLocal("Odczyt", "Read"), trLocal("Zapis", "Write"), trLocal("Wykonanie / przechodzenie", "Execute / traverse")};
        const QStringList rows{trLocal("Właściciel", "Owner"), trLocal("Grupa / maska ACL", "Group / ACL mask"), trLocal("Inni", "Others")};
        for (int c = 0; c < 3; ++c) grid->addWidget(new QLabel(columns[c], group), 0, c + 1);
        for (int row = 0; row < 3; ++row) {
            grid->addWidget(new QLabel(rows[row], group), row + 1, 0);
            for (int col = 0; col < 3; ++col) {
                const int i = row * 3 + col;
                auto *cell = new QWidget(group); auto *cellLayout = new QVBoxLayout(cell);
                cellLayout->setContentsMargins(0, 0, 0, 0);
                current[i] = new QLabel(cell); current[i]->setObjectName(QStringLiteral("bulkCurrent%1").arg(i));
                choices[i] = new QComboBox(cell); choices[i]->setObjectName(QStringLiteral("bulkChange%1").arg(i));
                choices[i]->addItems({trLocal("Nie zmieniaj", "Do not change"), trLocal("Ustaw", "Set"), trLocal("Usuń", "Clear")});
                cellLayout->addWidget(current[i]); cellLayout->addWidget(choices[i]); grid->addWidget(cell, row + 1, col + 1);
                connect(choices[i], &QComboBox::currentIndexChanged, this, [this] { if (stateChanged) stateChanged(); });
            }
        }
        layout->addWidget(group);
        status = new QLabel(this); status->setWordWrap(true); status->setObjectName(QStringLiteral("bulkPermissionStatus")); layout->addWidget(status);
        details = new QPlainTextEdit(this); details->setReadOnly(true); details->setObjectName(QStringLiteral("bulkPermissionResults")); layout->addWidget(details);
        auto *reset = new QPushButton(trLocal("Nie zmieniaj żadnego bitu", "Do not change any bit"), this);
        connect(reset, &QPushButton::clicked, this, [this] { if (!busy) resetEdit(); }); layout->addWidget(reset);
        refreshDisplay(); setReady(false);
    }
    ~AggregatePermissionsWidget() override { token->store(true); }
    std::function<void()> stateChanged;
    std::function<void(bool)> applied;
    bool isBusy() const { return busy; }
    bool canApply() const { return ready && !stopped && !busy && model.allEditable() && edit().changeMask; }
    AggregatePermissionEdit edit() const {
        AggregatePermissionEdit value;
        for (int i = 0; i < 9; ++i) {
            const mode_t bit = 0400 >> i;
            if (choices[i]->currentIndex()) value.changeMask |= bit;
            if (choices[i]->currentIndex() == 1) value.valueMask |= bit;
        }
        return value;
    }
    void setReady(bool value) { ready = value; updateEnabled(); }
    void stop() { stopped = true; token->store(true); resetEdit(); updateEnabled(); }
    void invalidate() { stop(); status->setText(trLocal("Zaznaczenie zmieniło się. Otwórz Właściwości ponownie.", "Selection changed. Reopen Properties.")); }
    void requestApply() {
        if (!canApply()) return;
        auto &gate = BatchRenameRecoveryGate::instance(); gate.refresh();
        if (gate.mutationsBlocked()) { status->setText(gate.message()); return; }
        const auto change = edit();
        auto *confirmation = new QMessageBox(QMessageBox::Warning, trLocal("Potwierdź zmianę uprawnień", "Confirm permission change"),
            trLocal("Zmienić wybrane bity rwx dla %1 pozycji? Bez rekurencji. Maska zmian: %2, maska wartości: %3. Zmiana grupy może zmienić efektywne prawa ACL. Operacje nie są atomowe.",
                    "Change selected rwx bits for %1 entries? No recursion. Change mask: %2, value mask: %3. Group changes may affect effective ACL rights. Operations are not atomic.")
                .arg(model.items.size()).arg(QString::number(change.changeMask, 8)).arg(QString::number(change.valueMask, 8)),
            QMessageBox::Yes | QMessageBox::No, this);
        confirmation->setObjectName(QStringLiteral("bulkPermissionConfirmation"));
        confirmation->setDefaultButton(QMessageBox::No);
        QStringList preview;
        for (const auto &item : model.items) preview << item.root.url.toDisplayString() + QStringLiteral(" : %1 → %2")
            .arg(QString::number(item.root.identity.st_mode & 07777, 8), QString::number(change.merged(item.root.identity.st_mode), 8));
        confirmation->setDetailedText(preview.join(QLatin1Char('\n')));
        QPointer<AggregatePermissionsWidget> alive(this);
        const int response = confirmation->exec();
        if (!alive) return;
        confirmation->setObjectName(QString());
        confirmation->deleteLater();
        if (response != QMessageBox::Yes || !canApply()) return;
        busy = true; token->store(false); updateEnabled();
        auto *watcher = new QFutureWatcher<QList<AggregatePermissionResult>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            const auto results = watcher->result(); watcher->deleteLater();
            busy = false;
            bool success = !results.isEmpty(); int changed = 0; QStringList lines;
            for (const auto &result : results) {
                success &= result.success; changed += result.changed;
                lines << result.url.toDisplayString() + QStringLiteral(" : ") + result.message;
            }
            details->setPlainText(lines.join(QLatin1Char('\n')));
            resetEdit();
            // Refresh modes only when the originally pinned identity/mount still
            // matches. Never adopt a replacement as the next Apply target.
            for (auto &item : model.items) {
                struct stat now {}, parent {};
                const int fd = ::open(item.root.path.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0 || ::fstat(fd, &now) || ::stat(AggregatePermissionsModel::parentPath(item.root).constData(), &parent)
                    || now.st_dev != item.root.identity.st_dev || now.st_ino != item.root.identity.st_ino
                    || (now.st_mode & S_IFMT) != (item.root.identity.st_mode & S_IFMT)
                    || parent.st_dev != item.parent.st_dev || parent.st_ino != item.parent.st_ino
                    || QFileInfo(QString::fromLocal8Bit(AggregatePermissionsModel::parentPath(item.root))).canonicalFilePath() != item.canonicalParent
                    || AggregatePermissionsModel::mountId(fd) != item.mountId
                    || AggregatePermissionsModel::mountRecord(item.mountId) != item.mountRecord) {
                    item.reason = QStringLiteral("Identity or mount changed; reopen Properties"); success = false;
                } else item.root.identity = now;
                if (fd >= 0) ::close(fd);
            }
            if (!success) stopped = true; // Partial failure requires a fresh selection.
            refreshDisplay();
            status->setText(trLocal("Zmieniono: %1 z %2. %3", "Changed: %1 of %2. %3")
                .arg(changed).arg(results.size()).arg(success ? trLocal("Zweryfikowano.", "Verified.") : trLocal("Wynik częściowy / błąd — sprawdź każdą pozycję i otwórz ponownie.", "Partial result / failure — inspect each item and reopen.")));
            updateEnabled(); if (applied) applied(success);
        });
        watcher->setFuture(QtConcurrent::run([snapshot = model, change, cancel = token] { return AggregatePermissionsWriter::apply(snapshot, change, cancel); }));
    }
private:
    void resetEdit() { for (auto *box : choices) box->setCurrentIndex(0); }
    void updateEnabled() {
        for (auto *box : choices) box->setEnabled(ready && !stopped && !busy && model.allEditable());
        if (stateChanged) stateChanged();
    }
    void refreshDisplay() {
        QStringList blocked;
        for (const auto &item : model.items) if (!item.editable()) blocked << item.root.url.toDisplayString() + QStringLiteral(" : ") + item.reason;
        for (int i = 0; i < 9; ++i) {
            const int bit = model.currentBit(0400 >> i);
            current[i]->setText(bit < 0 ? trLocal("Bieżące: mieszane", "Current: mixed") : bit ? trLocal("Bieżące: ustawione", "Current: set") : trLocal("Bieżące: wyłączone", "Current: unset"));
        }
        status->setText(blocked.isEmpty() ? trLocal("Zmiany wymagają jawnego potwierdzenia. Zapis dostępny po zakończeniu skanowania.", "Changes require explicit confirmation. Editing is available after scanning.")
            : trLocal("Cały zestaw tylko do odczytu: zawiera nieobsługiwane pozycje.", "Entire selection is read-only: contains unsupported entries."));
        if (!blocked.isEmpty()) details->setPlainText(blocked.join(QLatin1Char('\n')));
    }
    AggregatePermissionsModel model;
    AggregatePermissionsWriter::Cancel token;
    std::array<QComboBox *, 9> choices{};
    std::array<QLabel *, 9> current{};
    QLabel *status;
    QPlainTextEdit *details;
    bool ready = false, stopped = false, busy = false;
};
