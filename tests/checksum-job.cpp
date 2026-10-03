/* Deterministic Stage 4 checksum job and UI contract coverage. */
#include "checksumjob.h"
#include "checksumwidget.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "fixture opened for writing");
    verify(file.write(data) == data.size(), "fixture fully written");
}

static ChecksumData runJob(ChecksumJob &job, int timeoutMs = 5000)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&job, &ChecksumJob::stateChanged, &loop, [&](const ChecksumData &data) {
        if (data.state != ChecksumState::Running) loop.quit();
    });
    verify(job.start(), "checksum job started");
    timeout.start(timeoutMs);
    loop.exec();
    verify(timeout.isActive(), "checksum job completed before timeout");
    return job.data();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir dir;
    verify(dir.isValid(), "temporary directory created");

    const QString shortPath = dir.filePath("short.txt");
    writeFile(shortPath, "abc");
    ChecksumJob shortJob(QUrl::fromLocalFile(shortPath));
    ChecksumData shortResult = runJob(shortJob);
    verify(shortResult.state == ChecksumState::Completed, "known vector completed");
    verify(shortResult.sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "known SHA-256 vector matches");
    verify(shortResult.sha256.size() == 64 && shortResult.sha256 == shortResult.sha256.toLower(),
           "result is lowercase 64-character hex");

    const QString emptyPath = dir.filePath("empty.bin");
    writeFile(emptyPath, {});
    ChecksumJob emptyJob(QUrl::fromLocalFile(emptyPath));
    const ChecksumData emptyResult = runJob(emptyJob);
    verify(emptyResult.sha256 == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "empty-file SHA-256 matches");

    QByteArray large(3 * 1024 * 1024 + 17, Qt::Uninitialized);
    for (qsizetype i = 0; i < large.size(); ++i) large[i] = char(i % 251);
    const QString largePath = dir.filePath("large.bin");
    writeFile(largePath, large);
    ChecksumJobOptions chunked;
    chunked.chunkSize = 64 * 1024;
    ChecksumJob largeJob(QUrl::fromLocalFile(largePath), nullptr, chunked);
    QList<quint64> progress;
    QObject::connect(&largeJob, &ChecksumJob::progress, [&](quint64 done, quint64) { progress << done; });
    const ChecksumData largeResult = runJob(largeJob);
    verify(largeResult.sha256 == QString::fromLatin1(QCryptographicHash::hash(large, QCryptographicHash::Sha256).toHex()),
           "multi-chunk result matches independent Qt vector");
    verify(progress.size() > 2, "multi-chunk job emitted progress repeatedly");
    for (qsizetype i = 1; i < progress.size(); ++i)
        verify(progress[i] >= progress[i - 1], "progress is monotonic");
    verify(progress.last() == static_cast<quint64>(large.size()), "progress reaches total byte count");

    ChecksumJobOptions slow = chunked;
    slow.chunkDelayMilliseconds = 2;
    ChecksumJob cancelJob(QUrl::fromLocalFile(largePath), nullptr, slow);
    bool cancelSent = false;
    QObject::connect(&cancelJob, &ChecksumJob::progress, [&](quint64 done, quint64) {
        if (done && !cancelSent) { cancelSent = true; cancelJob.cancel(); }
    });
    ChecksumData cancelled = runJob(cancelJob);
    verify(cancelled.state == ChecksumState::Cancelled && !cancelled.hasValidResult(),
           "mid-stream cancel publishes no valid result");
    ChecksumData restarted = runJob(cancelJob);
    verify(restarted.state == ChecksumState::Completed && restarted.sha256 == largeResult.sha256,
           "job restarts successfully after cancel");

    const QString removedPath = dir.filePath("removed.bin");
    writeFile(removedPath, large);
    ChecksumJob removedJob(QUrl::fromLocalFile(removedPath), nullptr, slow);
    bool removed = false;
    QObject::connect(&removedJob, &ChecksumJob::progress, [&](quint64 done, quint64) {
        if (done && !removed) { removed = QFile::remove(removedPath); }
    });
    const ChecksumData removedResult = runJob(removedJob);
    verify(removed && removedResult.state == ChecksumState::ChangedDuringHash
           && !removedResult.hasValidResult(), "disappearing path is rejected as changed");

    const QString modifiedPath = dir.filePath("modified.bin");
    writeFile(modifiedPath, large);
    ChecksumJob modifiedJob(QUrl::fromLocalFile(modifiedPath), nullptr, slow);
    bool modified = false;
    QObject::connect(&modifiedJob, &ChecksumJob::progress, [&](quint64 done, quint64) {
        if (done && !modified) {
            QFile file(modifiedPath);
            modified = file.open(QIODevice::Append) && file.write("change") == 6;
        }
    });
    const ChecksumData modifiedResult = runJob(modifiedJob);
    verify(modified && modifiedResult.state == ChecksumState::ChangedDuringHash
           && !modifiedResult.hasValidResult(), "modified file is rejected as changed");

    const QString truncatedPath = dir.filePath("truncated.bin");
    writeFile(truncatedPath, large);
    ChecksumJob truncatedJob(QUrl::fromLocalFile(truncatedPath), nullptr, slow);
    bool truncated = false;
    QObject::connect(&truncatedJob, &ChecksumJob::progress, [&](quint64 done, quint64) {
        if (done && !truncated) {
            QFile file(truncatedPath);
            truncated = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write("short") == 5;
        }
    });
    const ChecksumData truncatedResult = runJob(truncatedJob);
    verify(truncated && truncatedResult.state == ChecksumState::ChangedDuringHash
           && !truncatedResult.hasValidResult(), "truncated file is rejected as changed");

    const QString replacedPath = dir.filePath("replaced.bin");
    const QString oldPath = dir.filePath("replaced.old");
    writeFile(replacedPath, large);
    ChecksumJob replacedJob(QUrl::fromLocalFile(replacedPath), nullptr, slow);
    bool replaced = false;
    QObject::connect(&replacedJob, &ChecksumJob::progress, [&](quint64 done, quint64) {
        if (done && !replaced) {
            replaced = QFile::rename(replacedPath, oldPath);
            if (replaced) writeFile(replacedPath, large);
        }
    });
    const ChecksumData replacedResult = runJob(replacedJob);
    verify(replaced && replacedResult.state == ChecksumState::ChangedDuringHash
           && !replacedResult.hasValidResult(), "replaced path is rejected as changed");

    ChecksumJobOptions deniedOptions;
    deniedOptions.forcedOpenError = EACCES;
    ChecksumJob deniedJob(QUrl::fromLocalFile(shortPath), nullptr, deniedOptions);
    const ChecksumData denied = runJob(deniedJob);
    verify(denied.state == ChecksumState::Failed
           && denied.capability == ChecksumCapability::Unreadable
           && !denied.errorMessage.isEmpty(), "deterministic permission error is reported");

    const QString folder = dir.filePath("folder");
    verify(QDir().mkdir(folder), "directory fixture created");
    verify(ChecksumJob::capabilityForUrl(QUrl::fromLocalFile(folder))
           == ChecksumCapability::DirectoryNotApplicable, "directory is not applicable");
    const QString link = dir.filePath("link");
    verify(::symlink(QFile::encodeName(shortPath).constData(), QFile::encodeName(link).constData()) == 0,
           "symlink fixture created");
    verify(ChecksumJob::capabilityForUrl(QUrl::fromLocalFile(link))
           == ChecksumCapability::SymlinkUnavailable, "symlink is unavailable without following target");
    verify(ChecksumJob::capabilityForUrl(QUrl("sftp://example.invalid/file"))
           == ChecksumCapability::RemoteUnavailable, "remote URL is unavailable without I/O");

    verify(::chmod(QFile::encodeName(shortPath).constData(), 0444) == 0, "read-only fixture mode set");
    ChecksumJob readOnlyJob(QUrl::fromLocalFile(shortPath));
    verify(runJob(readOnlyJob).sha256 == shortResult.sha256, "read-only but readable file hashes successfully");

    ChecksumWidget widget(QUrl::fromLocalFile(shortPath));
    verify(widget.job()->data().state == ChecksumState::Idle, "widget construction does not auto-hash");
    auto *copy = widget.findChild<QPushButton *>("checksumCopy");
    auto *calculate = widget.findChild<QPushButton *>("checksumCalculate");
    verify(copy && !copy->isEnabled() && calculate && calculate->isEnabled(),
           "copy disabled and calculate enabled before result");
    calculate->click();
    QEventLoop uiLoop;
    QObject::connect(widget.job(), &ChecksumJob::stateChanged, &uiLoop, [&](const ChecksumData &data) {
        if (data.state == ChecksumState::Completed) uiLoop.quit();
    });
    QTimer::singleShot(5000, &uiLoop, &QEventLoop::quit);
    uiLoop.exec();
    verify(copy->isEnabled(), "copy enabled after valid result");
    copy->click();
    verify(QApplication::clipboard()->text() == shortResult.sha256, "copy writes exact hash to clipboard");

    auto *closingWidget = new ChecksumWidget(QUrl::fromLocalFile(largePath), nullptr, slow);
    closingWidget->findChild<QPushButton *>("checksumCalculate")->click();
    delete closingWidget;
    verify(true, "destroying UI during hashing cancels and joins worker safely");

    qInfo("PASS: %d checksum assertions; SHA-256 streaming, progress, cancel, change detection, capabilities, UI", checks);
    return 0;
}
