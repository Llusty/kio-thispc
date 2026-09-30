/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localfilemovejob.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>


static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

template<typename Predicate>
static bool waitUntil(Predicate predicate, int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(3);
    }
    return predicate();
}

static bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

static QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

static void waitForResult(QSignalSpy &spy, const char *description)
{
    verify(waitUntil([&spy] { return spy.count() == 1; }, 10000), description);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary move directory exists");

    QByteArray payload(8 * 1024 * 1024, Qt::Uninitialized);
    for (qsizetype index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>((index * 19 + 7) & 0xff);
    }
    const QByteArray expectedHash =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    const QString source = temporary.filePath(QStringLiteral("source.bin"));
    const QString destination = temporary.filePath(QStringLiteral("destination.bin"));
    verify(writeFile(source, payload), "cross-device-style source is created");

    LocalFileMoveJob move(
        QUrl::fromLocalFile(source), QUrl::fromLocalFile(destination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
    move.setAutoDelete(false);
    QSignalSpy moved(&move, &KJob::result);
    move.start();
    verify(waitUntil([&move] {
               return move.processedAmount(KJob::Bytes) >= 256 * 1024;
           }, 3000),
           "copy-before-remove move reports progress");
    verify(move.suspend(), "copy-before-remove move can be paused exactly");
    const qulonglong pausedBytes = move.processedAmount(KJob::Bytes);
    const QString partial = destination + QStringLiteral(".thispc-part");
    const qint64 pausedSize = QFileInfo(partial).size();
    QTest::qWait(120);
    verify(move.processedAmount(KJob::Bytes) == pausedBytes
               && QFileInfo(partial).size() == pausedSize
               && QFileInfo::exists(source),
           "pause freezes the partial and retains the source");
    verify(move.resume(), "paused move resumes");
    waitForResult(moved, "copy-before-remove move emits one result");
    verify(move.error() == KJob::NoError
               && move.moveError() == LocalFileMoveError::None
               && !move.usedAtomicMove(),
           "forced copy-before-remove move succeeds through native copy");
    verify(!QFileInfo::exists(source)
               && QFileInfo::exists(destination)
               && fileHash(destination) == expectedHash
               && !QFileInfo::exists(partial),
           "source is removed only after byte-exact destination publication");

    const QString cancelSource = temporary.filePath(QStringLiteral("cancel-source.bin"));
    const QString cancelDestination = temporary.filePath(QStringLiteral("cancel-destination.bin"));
    verify(writeFile(cancelSource, payload), "cancel source is created");
    LocalFileMoveJob cancelled(
        QUrl::fromLocalFile(cancelSource), QUrl::fromLocalFile(cancelDestination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledResult(&cancelled, &KJob::result);
    cancelled.start();
    verify(waitUntil([&cancelled] {
               return cancelled.processedAmount(KJob::Bytes) >= 128 * 1024;
           }, 3000),
           "cancelled move reaches active copy state");
    verify(cancelled.kill(KJob::EmitResult), "active move can be cancelled");
    waitForResult(cancelledResult, "cancelled move emits one result");
    verify(waitUntil([&] { return !QFileInfo::exists(cancelDestination + QStringLiteral(".thispc-part")); }, 3000)
               && QFileInfo::exists(cancelSource)
               && !QFileInfo::exists(cancelDestination),
           "cancelled move retains source and removes partial destination");

    const QString removeSource = temporary.filePath(QStringLiteral("remove-failure-source"));
    const QString removeDestination = temporary.filePath(QStringLiteral("remove-failure-destination"));
    verify(writeFile(removeSource, "retain-me"), "source-removal failure fixture is created");
    LocalFileMoveJob removeFailure(
        QUrl::fromLocalFile(removeSource), QUrl::fromLocalFile(removeDestination), nullptr,
        LocalFileMoveStrategy::ForceCopyAndRemove, 4096, 0,
        [](const QString &) { return false; });
    removeFailure.setAutoDelete(false);
    QSignalSpy removeFailureResult(&removeFailure, &KJob::result);
    removeFailure.start();
    waitForResult(removeFailureResult, "source-removal failure emits one result");
    verify(removeFailure.moveError() == LocalFileMoveError::SourceRemove
               && QFileInfo::exists(removeSource)
               && !QFileInfo::exists(removeDestination),
           "failed source removal rolls back the new destination");

    const QString atomicSource = temporary.filePath(QStringLiteral("atomic-source"));
    const QString atomicDestination = temporary.filePath(QStringLiteral("atomic-destination"));
    verify(writeFile(atomicSource, "atomic"), "same-filesystem source is created");
    LocalFileMoveJob atomic(
        QUrl::fromLocalFile(atomicSource), QUrl::fromLocalFile(atomicDestination));
    atomic.setAutoDelete(false);
    QSignalSpy atomicResult(&atomic, &KJob::result);
    atomic.start();
    waitForResult(atomicResult, "same-filesystem move emits one result");
    verify(atomic.error() == KJob::NoError && atomic.usedAtomicMove()
               && !QFileInfo::exists(atomicSource)
               && QFileInfo::exists(atomicDestination),
           "same-filesystem move uses atomic rename");

    LocalMoveHistory history;
    verify(!history.recordCompletedMove(
               QUrl::fromLocalFile(atomicSource),
               QUrl::fromLocalFile(temporary.filePath(QStringLiteral("missing")))),
           "history rejects an operation that did not complete");

    const QString historySource = temporary.filePath(QStringLiteral("history-source"));
    const QString historyDestination = temporary.filePath(QStringLiteral("history-destination"));
    verify(writeFile(historySource, "history"), "history source is created");
    LocalFileMoveJob recordedMove(
        QUrl::fromLocalFile(historySource), QUrl::fromLocalFile(historyDestination));
    recordedMove.setAutoDelete(false);
    verify(history.recordWhenCompleted(&recordedMove),
           "successful move is registered for completion tracking");
    QSignalSpy recordedResult(&recordedMove, &KJob::result);
    recordedMove.start();
    waitForResult(recordedResult, "tracked move completes");
    verify(history.count() == 1 && history.position() == 1
               && history.canUndo() && !history.canRedo(),
           "only the completed move enters native history");

    KJob *undo = history.undo();
    verify(undo != nullptr, "native Undo starts");
    QSignalSpy undoResult(undo, &KJob::result);
    waitForResult(undoResult, "native Undo completes");
    verify(QFileInfo::exists(historySource)
               && !QFileInfo::exists(historyDestination)
               && !history.canUndo() && history.canRedo(),
           "Undo reverses the move and advances history only after success");

    KJob *redo = history.redo();
    verify(redo != nullptr, "native Redo starts");
    QSignalSpy redoResult(redo, &KJob::result);
    waitForResult(redoResult, "native Redo completes");
    verify(!QFileInfo::exists(historySource)
               && QFileInfo::exists(historyDestination)
               && history.canUndo() && !history.canRedo(),
           "Redo reapplies the completed move");

    const QString failedSource = temporary.filePath(QStringLiteral("failed-source"));
    verify(writeFile(failedSource, "failed")
               && writeFile(temporary.filePath(QStringLiteral("occupied")), "occupied"),
           "failed tracked move fixture is created");
    LocalFileMoveJob failed(
        QUrl::fromLocalFile(failedSource),
        QUrl::fromLocalFile(temporary.filePath(QStringLiteral("occupied"))));
    failed.setAutoDelete(false);
    history.recordWhenCompleted(&failed);
    QSignalSpy failedResult(&failed, &KJob::result);
    failed.start();
    waitForResult(failedResult, "failed tracked move finishes");
    verify(history.count() == 1 && history.position() == 1,
           "failed move does not create an Undo record");

    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QString overwriteSource = temporary.filePath("overwrite-source");
    const QString overwriteDestination = temporary.filePath("overwrite-destination");
    verify(writeFile(overwriteSource, payload) && writeFile(overwriteDestination, "old destination"),
           "overwrite move fixtures are created");
    for (int scenario : {0, 1, 2}) {
        LocalFileMoveJob overwrite(
            QUrl::fromLocalFile(overwriteSource), QUrl::fromLocalFile(overwriteDestination), nullptr,
            LocalFileMoveStrategy::ForceCopyAndRemove, 16 * 1024, 1,
            [scenario](const QString &path) { return scenario != 1 && QFile::remove(path); }, true);
        overwrite.setAutoDelete(false);
        QSignalSpy result(&overwrite, &KJob::result);
        overwrite.start();
        verify(waitUntil([&] { return overwrite.processedAmount(KJob::Bytes) >= 128 * 1024; }, 3000),
               "overwrite move reaches chunk progress");
        verify(overwrite.suspend() && readFile(overwriteDestination) == "old destination",
               "overwrite move preserves the old destination while paused");
        verify(scenario == 0 ? overwrite.kill(KJob::EmitResult) : overwrite.resume(),
               "overwrite move accepts cancel or resume");
        waitForResult(result, "overwrite move emits one result");
        verify(waitUntil([&] {
                   return !QFileInfo::exists(overwriteDestination + QStringLiteral(".thispc-part"));
               }, 3000), "overwrite move cleans temporary and rollback data");
        if (scenario < 2) {
            verify(readFile(overwriteDestination) == "old destination"
                       && fileHash(overwriteSource) == expectedHash
                       && overwrite.moveError() == (scenario == 0
                           ? LocalFileMoveError::Cancelled : LocalFileMoveError::SourceRemove),
                   "cancel and source-removal failure retain source and original destination");
        } else {
            verify(overwrite.error() == 0 && !QFileInfo::exists(overwriteSource)
                       && fileHash(overwriteDestination) == expectedHash,
                   "successful overwrite move publishes before removing the source");
        }
    }

    qInfo("PASS: %d LocalFileMoveJob/history assertions", checks);
    return 0;
}
