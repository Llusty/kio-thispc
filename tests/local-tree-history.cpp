#include "localtreehistory.h"
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
    verify(QTest::qWaitFor([&] { return spy.count() == 1; }, 15000), "history job emits one result");
}
static KJob *waitHistory(KJob *job)
{
    verify(job != nullptr, "history job starts");
    QSignalSpy result(job, &KJob::result);
    waitResult(result);
    return job;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    const QString source = root.filePath("source");
    const QString destination = root.filePath("destination");
    verify(root.isValid() && QDir().mkpath(source + "/nested/empty") && QDir().mkdir(destination)
               && writeFile(source + "/nested/data", "payload")
               && ::symlink("nested/data", QFile::encodeName(source + "/link").constData()) == 0,
           "tree history fixtures are created");
    verify(::chmod(QFile::encodeName(source + "/nested").constData(), 0550) == 0,
           "copied directory is read-only");
    LocalTransferJob copy({QUrl::fromLocalFile(source)}, QUrl::fromLocalFile(destination), false);
    copy.setAutoDelete(false);
    QSignalSpy copyResult(&copy, &KJob::result);
    copy.start();
    waitResult(copyResult);
    LocalTreeHistory history;
    history.recordCompleted(&copy, 10);
    verify(copy.error() == 0 && history.canUndo() && history.undoSerial() == 10,
           "completed tree copy enters ordered history");
    verify(waitHistory(history.undo())->error() == 0 && !QFileInfo::exists(destination + "/source")
               && readFile(source + "/nested/data") == "payload" && history.canRedo(),
           "copy Undo removes only its tree including read-only created directories");
    verify(waitHistory(history.redo())->error() == 0
               && readFile(destination + "/source/nested/data") == "payload"
               && readLocalLink(destination + "/source/link") == "nested/data",
           "tree copy Redo restores files, empty directories and links");
    verify(writeFile(destination + "/source/unrelated", "keep"), "external destination item is added");
    verify(waitHistory(history.undo())->error() != 0
               && readFile(destination + "/source/unrelated") == "keep"
               && readFile(destination + "/source/nested/data") == "payload",
           "tree Undo refuses changed contents before removing recorded files");
    ::chmod(QFile::encodeName(source + "/nested").constData(), 0750);
    ::chmod(QFile::encodeName(destination + "/source/nested").constData(), 0750);

    const QString merge = root.filePath("merge");
    verify(QDir().mkpath(merge + "/source") && writeFile(merge + "/source/unrelated", "keep"),
           "move merges into a directory with an existing unrelated file");
    LocalTransferJob move({QUrl::fromLocalFile(source)}, QUrl::fromLocalFile(merge), true);
    move.setAutoDelete(false);
    QSignalSpy moveResult(&move, &KJob::result);
    move.start();
    waitResult(moveResult);
    LocalTreeHistory moves;
    moves.recordCompleted(&move, 20);
    verify(move.error() == 0 && moves.canUndo() && !QFileInfo::exists(source),
           "completed merged tree move enters history");
    verify(waitHistory(moves.undo())->error() == 0 && readFile(source + "/nested/data") == "payload"
               && readFile(merge + "/source/unrelated") == "keep"
               && !QFileInfo::exists(merge + "/source/nested"),
           "move Undo restores sources and preserves the pre-existing merged directory");
    verify(waitHistory(moves.redo())->error() == 0 && !QFileInfo::exists(source)
               && readFile(merge + "/source/nested/data") == "payload"
               && readFile(merge + "/source/unrelated") == "keep",
           "move Redo reapplies the exact plan without touching unrelated contents");
    qInfo("PASS: %d LocalTreeHistory assertions", checks);
    return 0;
}
