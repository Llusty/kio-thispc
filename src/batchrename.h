/*
 * Safe, local batch-rename planning and preview dialog.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#ifndef Q_OS_WIN
#include <sys/stat.h>
#endif
#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

enum class BatchRenameCase { Keep, Lower, Upper, Title };
enum class BatchRenameExtension { Keep, Replace, Remove };

struct BatchRenameOptions
{
    QString prefix;
    QString suffix;
    bool numbering = false;
    int firstNumber = 1;
    int padding = 2;
    QString findText;
    QString replacementText;
    bool regularExpression = false;
    BatchRenameCase letterCase = BatchRenameCase::Keep;
    BatchRenameExtension extensionMode = BatchRenameExtension::Keep;
    QString extension;
};

struct BatchRenameEntry
{
    QUrl source;
    QUrl destination;
    QString oldName;
    QString newName;
    QString problem;
    qint64 size = -1;
    QDateTime modified;
    bool directory = false;
    bool symlink = false;
    bool noOp = false;
#ifndef Q_OS_WIN
    quint64 sourceDevice = 0;
    quint64 sourceInode = 0;
#endif
};

struct BatchRenamePlan
{
    QList<BatchRenameEntry> entries;
    // Entries in executable order: vacate a selected destination before filling it.
    QList<int> executionOrder;
    // Exchanges are atomic individually. A cycle of 3+ names consists of
    // multiple exchanges and is NOT an atomic batch. An isolated completed
    // cycle may have a separately journaled, non-atomic history replay.
    QList<QPair<int, int>> atomicSwaps;
    QList<QList<int>> exchangeCycles;
    QString error;
    int errorRow = -1;

    int activeCount() const
    {
        return std::count_if(entries.cbegin(), entries.cend(),
                             [](const BatchRenameEntry &entry) { return !entry.noOp; });
    }

    bool isValid() const
    {
        int scheduled = executionOrder.size() + 2 * atomicSwaps.size();
        for (const auto &cycle : exchangeCycles) scheduled += cycle.size();
        const int active = activeCount();
        return error.isEmpty() && active > 0 && scheduled == active;
    }
};

inline bool batchRenameNameIsValid(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".")
        && name != QStringLiteral("..") && !name.contains(QLatin1Char('/'))
        && !name.contains(QChar::Null) && name.toUtf8().size() <= 255;
}

inline QString batchRenameTitleCase(QString text)
{
    bool wordStart = true;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isLetterOrNumber()) {
            text[i] = wordStart ? text.at(i).toUpper() : text.at(i).toLower();
            wordStart = false;
        } else {
            wordStart = true;
        }
    }
    return text;
}

inline void batchRenameSetError(BatchRenamePlan &plan, int row, const QString &message)
{
    if (row >= 0 && row < plan.entries.size()) plan.entries[row].problem = message;
    if (plan.error.isEmpty()) {
        plan.error = message;
        plan.errorRow = row;
    }
}

inline BatchRenamePlan makeBatchRenamePlan(QList<QUrl> urls, const BatchRenameOptions &options)
{
    BatchRenamePlan plan;
    if (urls.size() < 2) {
        plan.error = trLocal("Wybierz co najmniej dwa elementy.",
                             "Select at least two items.");
        return plan;
    }
    if (urls.size() > 1000) {
        plan.error = trLocal(
            "Jedna partia może zawierać najwyżej 1000 elementów.",
            "A batch may contain at most 1000 items.");
        return plan;
    }
    if (options.regularExpression && options.findText.isEmpty()) {
        plan.error = trLocal("Wzorzec wyrażenia regularnego nie może być pusty.",
                             "The regular-expression pattern cannot be empty.");
        return plan;
    }
    if (options.findText.size() > 1024 || options.replacementText.size() > 4096) {
        plan.error = trLocal("Wzorzec lub tekst zamiany przekracza bezpieczny limit.",
                             "The pattern or replacement exceeds the safety limit.");
        return plan;
    }
    QRegularExpression expression;
    if (options.regularExpression) {
        expression.setPattern(options.findText);
        if (!expression.isValid()) {
            plan.error = trLocal("Nieprawidłowe wyrażenie regularne: ",
                                 "Invalid regular expression: ") + expression.errorString();
            return plan;
        }
    }
    std::sort(urls.begin(), urls.end(), [](const QUrl &left, const QUrl &right) {
        return left.toEncoded(QUrl::FullyEncoded) < right.toEncoded(QUrl::FullyEncoded);
    });

    QString parent;
    QSet<QString> sources;
    QSet<QString> sourceKeys;
    QHash<QString, int> sourceRows;
    for (const QUrl &url : std::as_const(urls)) {
        if (!url.isLocalFile()) {
            plan.error = trLocal(
                "Obsługiwane są wyłącznie lokalne pliki i katalogi.",
                "Only local files and folders are supported.");
            return plan;
        }
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() && !info.isSymLink()) {
            plan.error = trLocal("Jeden z wybranych elementów już nie istnieje.",
                                 "One of the selected items no longer exists.");
            return plan;
        }
        const QString itemParent = QDir::cleanPath(info.absolutePath());
        if (parent.isEmpty()) parent = itemParent;
        if (parent != itemParent) {
            plan.error = trLocal("Wszystkie elementy muszą pochodzić z jednego katalogu.",
                                 "All items must come from one directory.");
            return plan;
        }
        const QString path = QDir::cleanPath(info.absoluteFilePath());
        if (sources.contains(path)) {
            plan.error = trLocal("Ten sam element wybrano więcej niż raz.",
                                 "The same item was selected more than once.");
            return plan;
        }
        sourceRows.insert(path, sourceRows.size());
        sources.insert(path);
        sourceKeys.insert(path.toCaseFolded());
    }

    // Build all rows before validation so an error never empties the preview.
    for (qsizetype index = 0; index < urls.size(); ++index) {
        const QFileInfo info(urls.at(index).toLocalFile());
        const QString oldName = info.fileName();
#ifndef Q_OS_WIN
        struct stat sourceStat {};
        if (::lstat(QFile::encodeName(info.absoluteFilePath()).constData(), &sourceStat) != 0) {
            plan.error = trLocal("Nie udało się sprawdzić tożsamości wybranego elementu.",
                                 "Could not verify a selected item's identity.");
            return plan;
        }
#endif
        QString base = oldName;
        QString extension;
        if (!info.isDir() && !oldName.startsWith(QLatin1Char('.'))) {
            const int dot = oldName.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) {
                base = oldName.left(dot);
                extension = oldName.mid(dot + 1);
            }
        }
        if (!options.findText.isEmpty()) {
            if (options.regularExpression) base.replace(expression, options.replacementText);
            else base.replace(options.findText, options.replacementText, Qt::CaseSensitive);
        }
        switch (options.letterCase) {
        case BatchRenameCase::Lower: base = base.toLower(); break;
        case BatchRenameCase::Upper: base = base.toUpper(); break;
        case BatchRenameCase::Title: base = batchRenameTitleCase(base); break;
        case BatchRenameCase::Keep: break;
        }
        if (!info.isDir()) {
            if (options.extensionMode == BatchRenameExtension::Remove) extension.clear();
            else if (options.extensionMode == BatchRenameExtension::Replace) {
                extension = options.extension.trimmed();
                while (extension.startsWith(QLatin1Char('.'))) extension.remove(0, 1);
            }
        }
        QString number;
        if (options.numbering) {
            number = QStringLiteral(" %1").arg(options.firstNumber + index, options.padding,
                                                10, QLatin1Char('0'));
        }
        const QString newName = options.prefix + base + options.suffix + number
            + (extension.isEmpty() ? QString() : QStringLiteral(".") + extension);
        const QString destinationPath = QDir(parent).filePath(newName);
        plan.entries.push_back({urls.at(index), QUrl::fromLocalFile(destinationPath),
                                oldName, newName, {}, info.size(), info.lastModified(),
                                info.isDir(), info.isSymLink(), newName == oldName
#ifndef Q_OS_WIN
                                , static_cast<quint64>(sourceStat.st_dev),
                                static_cast<quint64>(sourceStat.st_ino)
#endif
                                });
    }

    QHash<QString, int> destinations;
    // dependency[row] is the selected item currently occupying this row's target.
    // Acyclic chains execute destination-first. Cycles use Linux exchanges;
    // 3+ cycles need one exchange per edge except the closing edge.
    QList<int> dependency(plan.entries.size(), -1);
    for (int row = 0; row < plan.entries.size(); ++row) {
        BatchRenameEntry &entry = plan.entries[row];
        const QString &newName = entry.newName;
        if (!batchRenameNameIsValid(newName)) {
            batchRenameSetError(plan, row,
                trLocal("Wiersz %1: wynik zawiera pustą, zbyt długą lub niedozwoloną nazwę.",
                        "Row %1: the result is empty, too long, or invalid.").arg(row + 1));
            continue;
        }
        if (entry.noOp) continue;
        const QString destinationPath = QDir::cleanPath(entry.destination.toLocalFile());
        const QString collisionKey = destinationPath.toCaseFolded();
        if (destinations.contains(collisionKey)) {
            const int other = destinations.value(collisionKey);
            const QString message = trLocal("Wiersze %1 i %2 tworzą tę samą nazwę: %3",
                                            "Rows %1 and %2 produce the same name: %3")
                .arg(other + 1).arg(row + 1).arg(newName);
            batchRenameSetError(plan, other, message);
            entry.problem = message;
        } else {
            destinations.insert(collisionKey, row);
        }
        const QFileInfo destination(destinationPath);
        const QString sourcePath = QDir::cleanPath(entry.source.toLocalFile());
        const bool exactOwnPath = destinationPath == sourcePath;
        const bool foldedOwnPath = collisionKey == sourcePath.toCaseFolded();
        if ((destination.exists() || destination.isSymLink())
            && !exactOwnPath && !sources.contains(destinationPath)) {
            batchRenameSetError(plan, row,
                trLocal("Wiersz %1: nazwa docelowa koliduje z istniejącym elementem spoza zaznaczenia: %2",
                        "Row %1: the destination conflicts with an unselected existing item: %2")
                    .arg(row + 1).arg(newName));
        } else if (!exactOwnPath && sources.contains(destinationPath)) {
            // The target belongs to this snapshot, and its row will vacate it.
            // We do not assume that a similarly spelled, case-folded path is
            // the same inode on a case-insensitive or mixed-case filesystem.
            const int owner = sourceRows.value(destinationPath);
            if (plan.entries.at(owner).noOp) {
                batchRenameSetError(plan, row,
                    trLocal("Wiersz %1: wynik koliduje z innym zaznaczonym elementem: %2",
                            "Row %1: the result conflicts with another selected item: %2")
                        .arg(row + 1).arg(newName));
            } else {
                dependency[row] = owner;
            }
        } else if (!exactOwnPath && !foldedOwnPath && sourceKeys.contains(collisionKey)) {
            batchRenameSetError(plan, row,
                trLocal("Wiersz %1: wynik koliduje z innym zaznaczonym elementem: %2",
                        "Row %1: the result conflicts with another selected item: %2")
                    .arg(row + 1).arg(newName));
        }
    }
    if (!plan.error.isEmpty()) return plan;
    if (plan.activeCount() == 0) {
        plan.error = trLocal("Żadna z wybranych nazw nie uległaby zmianie.",
                             "None of the selected names would change.");
        return plan;
    }

    // Deterministic vacancy-first ordering for chains. Isolated cycles on
    // Linux exchange occupied directory entries without ever removing any.
    // A cycle of 3+ is not globally atomic: crash can leave an intermediate
    // permutation. The caller must warn and must roll back on normal failures.
    QList<bool> moved(plan.entries.size(), false);
    for (int row = 0; row < plan.entries.size(); ++row)
        moved[row] = plan.entries.at(row).noOp;
    for (int pass = 0; pass < plan.activeCount();) {
        int ready = -1;
        for (int row = 0; row < plan.entries.size(); ++row) {
            if (!moved.at(row) && (dependency.at(row) < 0 || moved.at(dependency.at(row)))) {
                ready = row;
                break;
            }
        }
        if (ready >= 0) {
            moved[ready] = true;
            plan.executionOrder.push_back(ready);
            ++pass;
            continue;
        }
        int first = -1;
        for (int row = 0; row < plan.entries.size(); ++row) {
            if (!moved.at(row)) { first = row; break; }
        }
        if (first < 0) break;
        // Each selected destination has a unique owner (otherwise validation
        // above fails), so the remaining component is an isolated cycle.
        QList<int> cycle;
        int cursor = first;
        do {
            if (cursor < 0 || cursor >= moved.size() || moved.at(cursor)
                || cycle.contains(cursor)) break;
            cycle.push_back(cursor);
            cursor = dependency.at(cursor);
        } while (cursor != first);
#ifdef Q_OS_LINUX
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
        bool exactCycle = cursor == first && cycle.size() >= 2;
        for (int i = 0; exactCycle && i < cycle.size(); ++i) {
            exactCycle = plan.entries.at(cycle.at(i)).destination
                == plan.entries.at(cycle.at((i + 1) % cycle.size())).source;
        }
        if (exactCycle) {
            for (int row : std::as_const(cycle)) moved[row] = true;
            pass += cycle.size();
            if (cycle.size() == 2) plan.atomicSwaps.push_back(qMakePair(cycle.at(0), cycle.at(1)));
            else plan.exchangeCycles.push_back(cycle);
            continue;
        }
#endif
#endif
        for (int row = 0; row < plan.entries.size(); ++row) {
            if (moved.at(row)) continue;
            batchRenameSetError(plan, row,
                trLocal("Wiersz %1: nieobsługiwany cykl nazw lub brak obsługi atomowej wymiany na tym systemie.",
                        "Row %1: unsupported name cycle or atomic exchange unavailable on this system.")
                    .arg(row + 1));
        }
        plan.executionOrder.clear();
        plan.atomicSwaps.clear();
        plan.exchangeCycles.clear();
        return plan;
    }
    return plan;
}

inline BatchRenamePlan makeBatchRenamePlan(QList<QUrl> urls, const QString &prefix,
                                           const QString &suffix, bool numbering,
                                           int firstNumber, int padding)
{
    BatchRenameOptions options;
    options.prefix = prefix;
    options.suffix = suffix;
    options.numbering = numbering;
    options.firstNumber = firstNumber;
    options.padding = padding;
    return makeBatchRenamePlan(std::move(urls), options);
}

inline bool batchRenameSourceUnchanged(const BatchRenameEntry &entry)
{
    const QFileInfo current(entry.source.toLocalFile());
#ifndef Q_OS_WIN
    struct stat sourceStat {};
    if (::lstat(QFile::encodeName(entry.source.toLocalFile()).constData(), &sourceStat) != 0
        || static_cast<quint64>(sourceStat.st_dev) != entry.sourceDevice
        || static_cast<quint64>(sourceStat.st_ino) != entry.sourceInode) return false;
#endif
    return (current.exists() || current.isSymLink())
        && current.fileName() == entry.oldName
        && current.isDir() == entry.directory
        && current.isSymLink() == entry.symlink
        && (entry.symlink || (current.size() == entry.size
                              && current.lastModified() == entry.modified));
}

inline bool batchRenameEntryReady(const BatchRenameEntry &entry)
{
    const QFileInfo destination(entry.destination.toLocalFile());
    return batchRenameSourceUnchanged(entry)
        && !destination.exists() && !destination.isSymLink();
}

// Recheck the complete snapshot before starting an acyclic dependency chain.
// A selected destination may exist only while its original occupant is still
// present and is itself scheduled to move. Per-step checks below remain required.
inline bool batchRenamePlanReady(const BatchRenamePlan &plan)
{
    if (!plan.isValid()) return false;
    QSet<QString> selected;
    for (const auto &entry : plan.entries) {
        if (!batchRenameSourceUnchanged(entry)) return false;
        selected.insert(QDir::cleanPath(entry.source.toLocalFile()));
    }
    for (const auto &entry : plan.entries) {
        const QString targetPath = QDir::cleanPath(entry.destination.toLocalFile());
        const QFileInfo target(targetPath);
        if ((target.exists() || target.isSymLink()) && !selected.contains(targetPath))
            return false;
    }
    return true;
}

// A two-way exchange never uses QFile::rename or a KIO overwrite option:
// the single Linux syscall exchanges TWO existing directory entries in place.
// No temporary name can be stranded by an application crash. The operation is
// Undo for one completed pair is handled by UndoController; an isolated
// multi-stage cycle can be replayed through its own journal, NOT atomically.
inline bool batchRenameSwapReady(const BatchRenamePlan &plan, int left, int right)
{
    if (!plan.isValid() || left < 0 || right < 0 || left >= plan.entries.size()
        || right >= plan.entries.size() || left == right) return false;
    const auto &a = plan.entries.at(left);
    const auto &b = plan.entries.at(right);
    return a.source.isLocalFile() && b.source.isLocalFile()
        && a.destination == b.source && b.destination == a.source
        && QDir::cleanPath(QFileInfo(a.source.toLocalFile()).absolutePath())
            == QDir::cleanPath(QFileInfo(b.source.toLocalFile()).absolutePath())
        && batchRenameSourceUnchanged(a) && batchRenameSourceUnchanged(b);
}

inline bool batchRenamePathHasOriginalIdentity(const BatchRenameEntry &entry, const QUrl &at)
{
#ifndef Q_OS_WIN
    struct stat identity {};
    if (!at.isLocalFile() || ::lstat(QFile::encodeName(at.toLocalFile()).constData(), &identity) != 0)
        return false;
    return static_cast<quint64>(identity.st_dev) == entry.sourceDevice
        && static_cast<quint64>(identity.st_ino) == entry.sourceInode;
#else
    Q_UNUSED(entry)
    Q_UNUSED(at)
    return false;
#endif
}

// Fail closed on ENOSYS/EOPNOTSUPP (unsupported kernel/filesystem). Every
// attempted exchange is one atomic syscall; we do not fall back to sequential
// moves. An unexpected post-exchange inode means fail closed: we never try to
// reverse an exchange involving an inode whose identity is unknown.
struct BatchRenameSwapWorkerResult {
    bool success = false;
    bool uncertain = false;
    QString error;
};

inline bool batchRenameAtomicSwap(const BatchRenamePlan &plan, int left, int right,
                                  QString *error = nullptr, bool *uncertain = nullptr)
{
    if (uncertain) *uncertain = false;
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!batchRenameSwapReady(plan, left, right))
        return fail(trLocal("Źródła zamiany zmieniły się od czasu podglądu.",
                            "Swap sources changed after the preview."));
#ifdef Q_OS_LINUX
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
    const auto &a = plan.entries.at(left);
    const auto &b = plan.entries.at(right);
    const QByteArray pathA = QFile::encodeName(a.source.toLocalFile());
    const QByteArray pathB = QFile::encodeName(b.source.toLocalFile());
    if (::syscall(SYS_renameat2, AT_FDCWD, pathA.constData(),
                  AT_FDCWD, pathB.constData(), RENAME_EXCHANGE) != 0) {
        return fail(trLocal("Atomowa zamiana nie powiodła się: ",
                            "Atomic exchange failed: ")
                    + QString::fromLocal8Bit(std::strerror(errno)));
    }
    if (batchRenamePathHasOriginalIdentity(a, b.source)
        && batchRenamePathHasOriginalIdentity(b, a.source)) return true;
    // A concurrent process interfered. Do not touch an unknown inode.
    if (uncertain) *uncertain = true;
    return fail(trLocal("Tożsamość plików zmieniła się podczas zamiany. Sprawdź obie nazwy ręcznie; nie wykonano kolejnych operacji.",
                        "File identities changed during the exchange. Inspect both names manually; no further operations were started."));
#else
    return fail(trLocal("Brak obsługi renameat2 w tej kompilacji.",
                        "renameat2 is unavailable in this build."));
#endif
#else
    return fail(trLocal("Atomowa zamiana wymaga systemu Linux.",
                        "Atomic exchange requires Linux."));
#endif
}

// Identity is captured before the original rename. Check both the directory
// entry itself and its original metadata, never dereferencing symlink payloads.
inline bool batchRenameSnapshotAt(const BatchRenameEntry &entry, const QUrl &path)
{
    if (!batchRenamePathHasOriginalIdentity(entry, path)) return false;
    const QFileInfo current(path.toLocalFile());
    return current.isDir() == entry.directory && current.isSymLink() == entry.symlink
        && (entry.symlink || (current.size() == entry.size
                              && current.lastModified() == entry.modified));
}

// Stage 3C.2A: replay a *single* completed two-way exchange.  This is one
// syscall, not a sequence of moves. The expected occupants are intentionally
// separate from the paths so the same function works for Undo and Redo.
// Fail closed if either entry is replaced or modified after the recorded swap.
inline bool batchRenameRecordedSwap(const BatchRenameEntry &expectedAtLeft,
                                    const QUrl &leftPath,
                                    const BatchRenameEntry &expectedAtRight,
                                    const QUrl &rightPath,
                                    QString *error = nullptr,
                                    bool *uncertain = nullptr)
{
    if (uncertain) *uncertain = false;
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!leftPath.isLocalFile() || !rightPath.isLocalFile() || leftPath == rightPath
        || QDir::cleanPath(QFileInfo(leftPath.toLocalFile()).absolutePath())
            != QDir::cleanPath(QFileInfo(rightPath.toLocalFile()).absolutePath())
        || !batchRenameSnapshotAt(expectedAtLeft, leftPath)
        || !batchRenameSnapshotAt(expectedAtRight, rightPath))
        return fail(trLocal("Zapisane pliki zamiany zostały zmienione; nie wykonano Undo/Redo.",
                            "Recorded swap files changed; Undo/Redo was not performed."));
#ifdef Q_OS_LINUX
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
    const QByteArray a = QFile::encodeName(leftPath.toLocalFile());
    const QByteArray b = QFile::encodeName(rightPath.toLocalFile());
    if (::syscall(SYS_renameat2, AT_FDCWD, a.constData(),
                  AT_FDCWD, b.constData(), RENAME_EXCHANGE) != 0)
        return fail(trLocal("Nie udało się zamienić nazw: ", "Name exchange failed: ")
                    + QString::fromLocal8Bit(std::strerror(errno)));
    if (batchRenameSnapshotAt(expectedAtLeft, rightPath)
        && batchRenameSnapshotAt(expectedAtRight, leftPath))
        return true;
    if (uncertain) *uncertain = true;
    return fail(trLocal("Nie można potwierdzić tożsamości po wymianie. Sprawdź obie nazwy ręcznie.",
                        "Identity check failed after exchange. Inspect both names manually."));
#else
    return fail(trLocal("Brak obsługi renameat2.", "renameat2 is unavailable."));
#endif
#else
    return fail(trLocal("Wymiana wymaga Linux.", "Exchange requires Linux."));
#endif
}

// State for one isolated 3+ cycle. 'occupants' maps original source paths
// (cycle order) to the original row/inode currently occupying each path.
// Every step exchanges two existing entries; there are NO temporary names.
// A power loss can leave a partial permutation. This is deliberately not
// advertised as an atomic transaction or automatic crash recovery.
struct BatchRenameCycleState
{
    QList<int> occupants;
    int stepsDone = 0;
    bool uncertain = false;
};

inline bool batchRenameCycleReady(const BatchRenamePlan &plan, const QList<int> &cycle)
{
    if (!plan.isValid() || cycle.size() < 3 || !plan.exchangeCycles.contains(cycle)) return false;
    QSet<int> unique;
    for (int row : cycle) {
        if (row < 0 || row >= plan.entries.size() || unique.contains(row)) return false;
        unique.insert(row);
    }
    for (int i = 0; i < cycle.size(); ++i) {
        const auto &entry = plan.entries.at(cycle.at(i));
        if (entry.destination != plan.entries.at(cycle.at((i + 1) % cycle.size())).source
            || !batchRenameSourceUnchanged(entry)) return false;
    }
    return true;
}

inline BatchRenameCycleState batchRenameCycleInitialState(const QList<int> &cycle)
{
    BatchRenameCycleState state;
    state.occupants = cycle;
    return state;
}

inline bool batchRenameCycleMatches(const BatchRenamePlan &plan, const QList<int> &cycle,
                                    const BatchRenameCycleState &state)
{
    if (!plan.isValid() || cycle.size() < 3 || !plan.exchangeCycles.contains(cycle)
        || state.uncertain || state.occupants.size() != cycle.size()
        || state.stepsDone < 0 || state.stepsDone >= cycle.size()) return false;
    QSet<int> unique;
    for (int position = 0; position < cycle.size(); ++position) {
        const int occupant = state.occupants.at(position);
        if (occupant < 0 || occupant >= plan.entries.size() || !cycle.contains(occupant)
            || unique.contains(occupant)) return false;
        unique.insert(occupant);
        if (!batchRenamePathHasOriginalIdentity(plan.entries.at(occupant),
                                                plan.entries.at(cycle.at(position)).source)) return false;
    }
    return true;
}

// Invokes the exact same Linux exchange primitive as the two-way swap, but
// checks the FULL cycle against its expected inode permutation before/after.
// If the postcheck fails, a concurrent process may have interfered; the
// caller must NEVER attempt a blind rollback over an unknown inode.
inline bool batchRenameCycleExchange(const BatchRenamePlan &plan, const QList<int> &cycle,
                                     BatchRenameCycleState &state, int step, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!batchRenameCycleMatches(plan, cycle, state))
        return fail(trLocal("Zmieniono tożsamość pliku w cyklu; wstrzymano wymianę.",
                            "A cycle file changed identity; exchange stopped."));
    if (step < 0 || step >= cycle.size() - 1)
        return fail(trLocal("Nieprawidłowy etap wymiany.", "Invalid exchange step."));
#ifdef Q_OS_LINUX
#if defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
    const QByteArray pivot = QFile::encodeName(plan.entries.at(cycle.at(0)).source.toLocalFile());
    const QByteArray other = QFile::encodeName(plan.entries.at(cycle.at(step + 1)).source.toLocalFile());
    if (::syscall(SYS_renameat2, AT_FDCWD, pivot.constData(),
                  AT_FDCWD, other.constData(), RENAME_EXCHANGE) != 0) {
        return fail(trLocal("Wymiana w cyklu nie powiodła się: ", "Cycle exchange failed: ")
                    + QString::fromLocal8Bit(std::strerror(errno)));
    }
    std::swap(state.occupants[0], state.occupants[step + 1]);
    if (!batchRenameCycleMatches(plan, cycle, state)) {
        state.uncertain = true;
        return fail(trLocal("Nieoczekiwana zmiana tożsamości w cyklu. Nie wolno automatycznie cofać; sprawdź nazwy ręcznie.",
                            "Unexpected identity change in cycle. Automatic rollback is unsafe; inspect names manually."));
    }
    return true;
#else
    return fail(trLocal("Brak obsługi renameat2 w tej kompilacji.",
                        "renameat2 is unavailable in this build."));
#endif
#else
    return fail(trLocal("Cykle wymagają Linux.", "Cycles require Linux."));
#endif
}

inline bool batchRenameCycleAdvance(const BatchRenamePlan &plan, const QList<int> &cycle,
                                    BatchRenameCycleState &state, QString *error = nullptr)
{
    if (state.stepsDone >= cycle.size() - 1)
        return false;
    if (!batchRenameCycleExchange(plan, cycle, state, state.stepsDone, error)) return false;
    ++state.stepsDone;
    return true;
}

inline bool batchRenameCycleRollback(const BatchRenamePlan &plan, const QList<int> &cycle,
                                     BatchRenameCycleState &state, QString *error = nullptr)
{
    if (state.uncertain) return false;
    while (state.stepsDone > 0) {
        if (!batchRenameCycleExchange(plan, cycle, state, state.stepsDone - 1, error)) return false;
        --state.stepsDone;
    }
    return batchRenameCycleMatches(plan, cycle, state);
}

// A history replay must check metadata as well as inode identity at EVERY
// occupied path. In particular, do not undo over a modified directory or link.
inline bool batchRenameCycleSnapshotsMatch(const BatchRenamePlan &plan,
                                           const QList<int> &cycle,
                                           const BatchRenameCycleState &state)
{
    if (!batchRenameCycleMatches(plan, cycle, state)) return false;
    for (int position = 0; position < cycle.size(); ++position) {
        const auto &expected = plan.entries.at(state.occupants.at(position));
        const auto &path = plan.entries.at(cycle.at(position)).source;
        if (!batchRenameSnapshotAt(expected, path)) return false;
    }
    return true;
}

// Build the expected final permutation without modifying the filesystem.
inline BatchRenameCycleState batchRenameCycleFinalState(const QList<int> &cycle)
{
    auto state = batchRenameCycleInitialState(cycle);
    for (int step = 0; step < cycle.size() - 1; ++step) {
        std::swap(state.occupants[0], state.occupants[step + 1]);
        ++state.stepsDone;
    }
    return state;
}

// Only a single, isolated cycle is eligible for a single history record.
// Chains, several cycles and mixed swaps retain their existing semantics.
inline bool batchRenameSingleCycleHistoryEligible(const BatchRenamePlan &plan)
{
    return plan.isValid() && plan.exchangeCycles.size() == 1
        && plan.atomicSwaps.isEmpty() && plan.executionOrder.isEmpty()
        && plan.activeCount() == plan.exchangeCycles.first().size()
        && plan.exchangeCycles.first().size() >= 3;
}

inline bool batchRenameCycleComplete(const BatchRenamePlan &plan, const QList<int> &cycle,
                                     const BatchRenameCycleState &state)
{
    if (state.stepsDone != cycle.size() - 1 || !batchRenameCycleMatches(plan, cycle, state))
        return false;
    for (int pos = 0; pos < cycle.size(); ++pos) {
        const auto &original = plan.entries.at(state.occupants.at(pos));
        if (original.destination != plan.entries.at(cycle.at(pos)).source) return false;
    }
    return true;
}

class BatchRenameDialog final : public QDialog
{
public:
    explicit BatchRenameDialog(const QList<QUrl> &urls, QWidget *parent = nullptr)
        : QDialog(parent), m_urls(urls)
    {
        setWindowTitle(trLocal("Zbiorcza zmiana nazw", "Batch Rename"));
        setMinimumSize(820, 620);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        m_prefix = new QLineEdit(this);
        m_suffix = new QLineEdit(this);
        m_find = new QLineEdit(this);
        m_replacement = new QLineEdit(this);
        m_regex = new QCheckBox(trLocal("Wyrażenie regularne", "Regular expression"), this);
        m_case = new QComboBox(this);
        m_case->addItems({trLocal("Bez zmian", "Keep"), trLocal("małe litery", "lowercase"),
                          trLocal("WIELKIE LITERY", "UPPERCASE"), trLocal("Jak Tytuł", "Title Case")});
        m_extensionMode = new QComboBox(this);
        m_extensionMode->addItems({trLocal("Zachowaj", "Keep"), trLocal("Zmień na", "Replace with"),
                                   trLocal("Usuń", "Remove")});
        m_extension = new QLineEdit(this);
        m_extension->setPlaceholderText(QStringLiteral("txt"));
        m_numbering = new QCheckBox(trLocal("Dodaj numer", "Add number"), this);
        m_first = new QSpinBox(this);
        m_first->setRange(0, 1000000000);
        m_first->setValue(1);
        m_padding = new QSpinBox(this);
        m_padding->setRange(1, 12);
        m_padding->setValue(2);
        form->addRow(trLocal("Prefiks:", "Prefix:"), m_prefix);
        form->addRow(trLocal("Sufiks:", "Suffix:"), m_suffix);
        form->addRow(trLocal("Znajdź:", "Find:"), m_find);
        form->addRow(trLocal("Zamień na:", "Replace with:"), m_replacement);
        form->addRow(m_regex);
        form->addRow(trLocal("Wielkość liter nazwy:", "Name letter case:"), m_case);
        form->addRow(trLocal("Rozszerzenie:", "Extension:"), m_extensionMode);
        form->addRow(trLocal("Nowe rozszerzenie:", "New extension:"), m_extension);
        form->addRow(m_numbering);
        form->addRow(trLocal("Pierwszy numer:", "First number:"), m_first);
        form->addRow(trLocal("Szerokość numeru:", "Number width:"), m_padding);
        layout->addLayout(form);
        m_table = new QTableWidget(this);
        m_table->setColumnCount(3);
        m_table->setHorizontalHeaderLabels({trLocal("Stara nazwa", "Old name"),
                                             trLocal("Nowa nazwa", "New name"),
                                             trLocal("Stan", "Status")});
        m_table->horizontalHeader()->setStretchLastSection(true);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(m_table, 1);
        m_message = new QLabel(this);
        m_message->setWordWrap(true);
        layout->addWidget(m_message);
        m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        m_buttons->button(QDialogButtonBox::Ok)->setText(trLocal("Zmień nazwy", "Rename"));
        layout->addWidget(m_buttons);
        connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        const auto update = [this] { rebuild(); };
        connect(m_prefix, &QLineEdit::textChanged, this, update);
        connect(m_suffix, &QLineEdit::textChanged, this, update);
        connect(m_find, &QLineEdit::textChanged, this, update);
        connect(m_replacement, &QLineEdit::textChanged, this, update);
        connect(m_regex, &QCheckBox::toggled, this, update);
        connect(m_case, &QComboBox::currentIndexChanged, this, update);
        connect(m_extensionMode, &QComboBox::currentIndexChanged, this, [this] { updateControls(); rebuild(); });
        connect(m_extension, &QLineEdit::textChanged, this, update);
        connect(m_numbering, &QCheckBox::toggled, this, [this] { updateControls(); rebuild(); });
        connect(m_first, &QSpinBox::valueChanged, this, update);
        connect(m_padding, &QSpinBox::valueChanged, this, update);
        updateControls();
        rebuild();
    }

    BatchRenamePlan plan() const { return m_plan; }

private:
    BatchRenameOptions options() const
    {
        BatchRenameOptions value;
        value.prefix = m_prefix->text();
        value.suffix = m_suffix->text();
        value.findText = m_find->text();
        value.replacementText = m_replacement->text();
        value.regularExpression = m_regex->isChecked();
        value.letterCase = static_cast<BatchRenameCase>(m_case->currentIndex());
        value.extensionMode = static_cast<BatchRenameExtension>(m_extensionMode->currentIndex());
        value.extension = m_extension->text();
        value.numbering = m_numbering->isChecked();
        value.firstNumber = m_first->value();
        value.padding = m_padding->value();
        return value;
    }

    void updateControls()
    {
        m_extension->setEnabled(m_extensionMode->currentIndex()
                                == static_cast<int>(BatchRenameExtension::Replace));
        m_first->setEnabled(m_numbering->isChecked());
        m_padding->setEnabled(m_numbering->isChecked());
    }

    void rebuild()
    {
        m_plan = makeBatchRenamePlan(m_urls, options());
        m_table->setRowCount(m_plan.entries.size());
        for (int row = 0; row < m_plan.entries.size(); ++row) {
            m_table->setItem(row, 0, new QTableWidgetItem(m_plan.entries.at(row).oldName));
            m_table->setItem(row, 1, new QTableWidgetItem(m_plan.entries.at(row).newName));
            m_table->setItem(row, 2, new QTableWidgetItem(!m_plan.entries.at(row).problem.isEmpty()
                ? m_plan.entries.at(row).problem
                : m_plan.entries.at(row).noOp ? trLocal("Pominięto (bez zmian)", "Skipped (unchanged)")
                                              : trLocal("Gotowe", "Ready")));
            if (!m_plan.entries.at(row).problem.isEmpty()) {
                for (int column = 0; column < m_table->columnCount(); ++column)
                    m_table->item(row, column)->setBackground(palette().brush(QPalette::Highlight));
            }
        }
        if (m_plan.errorRow >= 0) {
            m_table->selectRow(m_plan.errorRow);
            m_table->scrollToItem(m_table->item(m_plan.errorRow, 0));
        }
        m_message->setText(!m_plan.error.isEmpty() ? m_plan.error
            : !m_plan.exchangeCycles.isEmpty()
                ? trLocal("Uwaga: cykl 3+ wymaga kilku osobnych atomowych wymian. Anulowanie lub błąd wywoła próbę rollbacku; awaria procesu może pozostawić częściowo zamienione nazwy. Nie ma Undo, a wcześniejsza historia zostanie unieważniona. Pracuj na kopiach.",
                          "Warning: a 3+ cycle needs multiple atomic exchanges. Cancel/error attempts rollback; a process crash may leave partially exchanged names. There is no Undo and prior history will be invalidated. Use copies.")
                : !m_plan.atomicSwaps.isEmpty()
                    ? trLocal("Uwaga: dwuelementowe zamiany są atomowe, ale bez Undo. Wykonanie unieważni wcześniejszą historię Undo.",
                              "Warning: two-way exchanges are atomic but have no Undo. Prior Undo history will be invalidated.")
                    : trLocal("Sprawdź podgląd. Pliki nie zostaną nadpisane. Dostępność wspólnego Undo zależy od rodzaju partii; szczegóły pojawią się przed wykonaniem.",
                              "Review the preview. Files will not be overwritten. Grouped Undo depends on the batch type; details appear before execution."));
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_plan.isValid());
    }

    QList<QUrl> m_urls;
    QLineEdit *m_prefix = nullptr;
    QLineEdit *m_suffix = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_replacement = nullptr;
    QCheckBox *m_regex = nullptr;
    QComboBox *m_case = nullptr;
    QComboBox *m_extensionMode = nullptr;
    QLineEdit *m_extension = nullptr;
    QCheckBox *m_numbering = nullptr;
    QSpinBox *m_first = nullptr;
    QSpinBox *m_padding = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_message = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    BatchRenamePlan m_plan;
};
