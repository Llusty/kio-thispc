#include "localtransferjob.h"
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

static void waitResult(QSignalSpy &spy)
{
    verify(QTest::qWaitFor([&] { return spy.count() == 1; }, 15000), "tree transfer emits one result");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    verify(root.isValid(), "disposable tree directory exists");
    const QString source = root.filePath("source");
    const QString destination = root.filePath("destination");
    verify(QDir().mkpath(source + "/nested/empty") && QDir().mkpath(destination),
           "source and destination directories are created");
    const QByteArray payload(2 * 1024 * 1024, 'x');
    verify(writeFile(source + "/first.bin", payload)
               && writeFile(source + "/nested/.hidden", "hidden")
               && writeFile(root.filePath("second.txt"), "second"), "multiple-source fixtures are created");
    const QList<QUrl> sources{QUrl::fromLocalFile(source), QUrl::fromLocalFile(root.filePath("second.txt"))};
    LocalTransferJob copy(sources, QUrl::fromLocalFile(destination), false, nullptr, 4096, 1);
    copy.setAutoDelete(false);
    QSignalSpy result(&copy, &KJob::result);
    QSignalSpy current(&copy, &LocalTransferJob::currentFileChanged);
    copy.start();
    verify(QTest::qWaitFor([&] { return copy.processedAmount(KJob::Bytes) >= 64 * 1024; }, 5000)
               && copy.suspend(), "tree copy pauses during file data transfer");
    const auto stopped = copy.processedAmount(KJob::Bytes);
    const auto partial = destination + "/source/first.bin.thispc-part";
    const auto stoppedSize = QFileInfo(partial).size();
    const QString parallelDestination = root.filePath("parallel");
    verify(QDir().mkpath(parallelDestination), "parallel destination is created");
    LocalTransferJob parallel(sources, QUrl::fromLocalFile(parallelDestination), false);
    parallel.setAutoDelete(false);
    QSignalSpy parallelResult(&parallel, &KJob::result);
    parallel.start();
    waitResult(parallelResult);
    verify(parallel.error() == 0 && copy.isSuspended(),
           "a second tree transfer completes independently while the first is paused");
    QTest::qWait(120);
    verify(copy.processedAmount(KJob::Bytes) == stopped && QFileInfo(partial).size() == stoppedSize,
           "tree pause freezes aggregate bytes and the active partial");
    verify(copy.resume(), "tree copy resumes");
    waitResult(result);
    verify(copy.error() == 0 && copy.totalAmount(KJob::Bytes) == static_cast<qulonglong>(payload.size() + 12)
               && copy.processedAmount(KJob::Bytes) == copy.totalAmount(KJob::Bytes),
           "tree copy reports aggregate progress for all sources");
    verify(readFile(destination + "/source/first.bin") == payload
               && readFile(destination + "/source/nested/.hidden") == "hidden"
               && readFile(destination + "/second.txt") == "second"
               && QDir(destination + "/source/nested/empty").exists()
               && readFile(source + "/first.bin") == payload && current.count() == 3,
           "tree copy preserves hidden files, empty directories, source data and current-file updates");

    const QString cancelDestination = root.filePath("cancel");
    verify(QDir().mkpath(cancelDestination), "cancellation destination exists");
    LocalTransferJob cancelled(sources, QUrl::fromLocalFile(cancelDestination), false, nullptr, 4096, 1);
    cancelled.setAutoDelete(false);
    QSignalSpy cancelledResult(&cancelled, &KJob::result);
    cancelled.start();
    verify(QTest::qWaitFor([&] { return cancelled.processedAmount(KJob::Bytes) >= 64 * 1024; }, 5000)
               && cancelled.suspend() && cancelled.kill(KJob::EmitResult),
           "active tree transfer can be cancelled while paused");
    waitResult(cancelledResult);
    verify(QTest::qWaitFor([&] {
               return !QFileInfo::exists(cancelDestination + "/source/first.bin.thispc-part");
           }, 5000) && !QFileInfo::exists(cancelDestination + "/source/first.bin")
               && readFile(source + "/first.bin") == payload,
           "tree cancellation removes the active partial and preserves untransferred sources");

    const QString movedDestination = root.filePath("moved");
    verify(QDir().mkpath(movedDestination), "move destination exists");
    LocalTransferJob move(sources, QUrl::fromLocalFile(movedDestination), true);
    move.setAutoDelete(false);
    QSignalSpy moved(&move, &KJob::result);
    move.start();
    waitResult(moved);
    verify(move.error() == 0 && !QFileInfo::exists(source) && !QFileInfo::exists(root.filePath("second.txt"))
               && readFile(movedDestination + "/source/first.bin") == payload
               && QDir(movedDestination + "/source/nested/empty").exists(),
           "tree move removes source directories only after moving their contents");

    LocalTransferJob conflict({QUrl::fromLocalFile(movedDestination + "/source")},
                              QUrl::fromLocalFile(destination), false);
    conflict.setAutoDelete(false);
    QSignalSpy conflictResult(&conflict, &KJob::result);
    conflict.start();
    waitResult(conflictResult);
    verify(conflict.error() != 0 && conflict.completedEntries().isEmpty()
               && readFile(destination + "/source/first.bin") == payload,
           "unresolved plan conflicts fail before execution touches files");

    LocalTransferJob cycle({QUrl::fromLocalFile(movedDestination)},
                           QUrl::fromLocalFile(movedDestination + "/source"), false);
    cycle.setAutoDelete(false);
    QSignalSpy cycleResult(&cycle, &KJob::result);
    cycle.start();
    waitResult(cycleResult);
    verify(cycle.error() != 0 && cycle.completedEntries().isEmpty(),
           "directory self-copy is rejected before planning or execution");
    LocalTransferJob duplicate(
        {QUrl::fromLocalFile(movedDestination + "/source"),
         QUrl::fromLocalFile(movedDestination + "/source/first.bin")},
        QUrl::fromLocalFile(cancelDestination), false);
    duplicate.setAutoDelete(false);
    QSignalSpy duplicateResult(&duplicate, &KJob::result);
    duplicate.start();
    waitResult(duplicateResult);
    verify(duplicate.error() != 0 && duplicate.completedEntries().isEmpty(),
           "overlapping source selections are rejected before any move or copy");
    const QString linkRoot = root.filePath("links");
    const QString linkDestination = root.filePath("link-destination");
    verify(QDir().mkpath(linkRoot + "/nested") && QDir().mkpath(linkDestination)
               && writeFile(linkRoot + "/nested/data", "link payload")
               && writeFile(root.filePath("external"), "external sentinel"),
           "symbolic-link and metadata fixtures are created");
    verify(::symlink("nested/data", QFile::encodeName(linkRoot + "/relative").constData()) == 0
               && ::symlink("absent", QFile::encodeName(linkRoot + "/broken").constData()) == 0
               && ::symlink("nested", QFile::encodeName(linkRoot + "/directory-link").constData()) == 0
               && ::symlink(QFile::encodeName(root.filePath("external")).constData(),
                            QFile::encodeName(linkRoot + "/outside").constData()) == 0,
           "relative, broken, directory and external links are created");
    const timespec expectedTimes[]{{1600000000, 123456789}, {1600003600, 987654321}};
    verify(::chmod(QFile::encodeName(linkRoot + "/nested").constData(), 0550) == 0
               && ::utimensat(AT_FDCWD, QFile::encodeName(linkRoot + "/nested").constData(), expectedTimes, 0) == 0
               && ::utimensat(AT_FDCWD, QFile::encodeName(linkRoot).constData(), expectedTimes, 0) == 0,
           "directory permissions and nanosecond timestamps are prepared");
    LocalTransferJob links({QUrl::fromLocalFile(linkRoot)}, QUrl::fromLocalFile(linkDestination), false);
    links.setAutoDelete(false);
    QSignalSpy linkResult(&links, &KJob::result);
    links.start();
    waitResult(linkResult);
    const QString copiedLinks = linkDestination + "/links";
    struct stat nestedStat {}, rootStat {};
    verify(links.error() == 0 && ::lstat(QFile::encodeName(copiedLinks + "/nested").constData(), &nestedStat) == 0
               && ::lstat(QFile::encodeName(copiedLinks).constData(), &rootStat) == 0
               && (nestedStat.st_mode & 0777) == 0550
               && nestedStat.st_atim.tv_sec == expectedTimes[0].tv_sec
               && nestedStat.st_atim.tv_nsec == expectedTimes[0].tv_nsec
               && nestedStat.st_mtim.tv_sec == expectedTimes[1].tv_sec
               && nestedStat.st_mtim.tv_nsec == expectedTimes[1].tv_nsec
               && rootStat.st_mtim.tv_nsec == expectedTimes[1].tv_nsec,
           "new directory metadata is restored after child creation with nanosecond precision");
    verify(readLocalLink(copiedLinks + "/relative") == "nested/data"
               && readLocalLink(copiedLinks + "/broken") == "absent"
               && QFileInfo(copiedLinks + "/broken").isSymLink()
               && readLocalLink(copiedLinks + "/directory-link") == "nested"
               && readLocalLink(copiedLinks + "/outside") == QFile::encodeName(root.filePath("external"))
               && readFile(root.filePath("external")) == "external sentinel"
               && links.transferPlan().symbolicLinkCount == 4
               && links.totalAmount(KJob::Bytes) == 12,
           "symbolic links retain their literal targets without traversing or copying target data");
    ::chmod(QFile::encodeName(linkRoot + "/nested").constData(), 0750);
    ::chmod(QFile::encodeName(copiedLinks + "/nested").constData(), 0750);

    const QString movedLinks = root.filePath("moved-links");
    verify(QDir().mkdir(movedLinks), "symbolic-link move destination is created");
    LocalTransferJob linkMove({QUrl::fromLocalFile(linkRoot)}, QUrl::fromLocalFile(movedLinks), true);
    linkMove.setAutoDelete(false);
    QSignalSpy linkMoved(&linkMove, &KJob::result);
    linkMove.start();
    waitResult(linkMoved);
    verify(linkMove.error() == 0 && !QFileInfo::exists(linkRoot)
               && readLocalLink(movedLinks + "/links/broken") == "absent"
               && readLocalLink(movedLinks + "/links/relative") == "nested/data"
               && readFile(root.filePath("external")) == "external sentinel",
           "tree move transfers links themselves and retains external target data");

    qInfo("PASS: %d LocalTransferJob assertions", checks);
    return 0;
}
