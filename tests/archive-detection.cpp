/*
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>

static int checks = 0;

static void verify(bool condition, const char *description)
{
    if (!condition) {
        qFatal("FAIL: %s", description);
    }

    ++checks;
}

static void createArchive(
    const QString &program,
    const QStringList &arguments,
    const QString &workingDirectory)
{
    const QString executable =
    QStandardPaths::findExecutable(program);

    verify(!executable.isEmpty(), "archive test tool is installed");

    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.start(executable, arguments);

    verify(
        process.waitForStarted(10000),
           "archive creation process starts");

    verify(
        process.waitForFinished(30000)
        && process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0,
           "archive creation succeeds");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir temporary;
    verify(temporary.isValid(), "temporary directory exists");

    const QString folder =
    temporary.filePath(QStringLiteral("folder z odstępem ż"));

    verify(
        QDir().mkpath(folder),
           "directory with spaces and Unicode exists");

    const QString source =
    QDir(folder).filePath(QStringLiteral("źródło.txt"));

    QFile input(source);

    verify(
        input.open(QIODevice::WriteOnly),
           "source file opens");

    verify(
        input.write("Archive detection test\n") > 0,
           "source file is written");

    input.close();

    const QString zip =
    QDir(folder).filePath(QStringLiteral("test ZIP.zip"));

    const QString sevenZip =
    QDir(folder).filePath(QStringLiteral("test 7z.7z"));

    const QString tarGz =
    QDir(folder).filePath(QStringLiteral("test tar.tar.gz"));

    createArchive(
        QStringLiteral("zip"),
                  {QStringLiteral("-q"), zip, QStringLiteral("źródło.txt")},
                  folder);

    createArchive(
        QStringLiteral("7z"),
                  {
                      QStringLiteral("a"),
                  QStringLiteral("-bd"),
                  QStringLiteral("-y"),
                  sevenZip,
                  QStringLiteral("źródło.txt")
                  },
                  folder);

    createArchive(
        QStringLiteral("tar"),
                  {
                      QStringLiteral("-czf"),
                  tarGz,
                  QStringLiteral("źródło.txt")
                  },
                  folder);

    verify(
        thispcCanExtractArchive(QUrl::fromLocalFile(zip), false),
           "ZIP is detected");

    verify(
        thispcCanExtractArchive(QUrl::fromLocalFile(sevenZip), false),
           "7z is detected");

    verify(
        thispcCanExtractArchive(QUrl::fromLocalFile(tarGz), false),
           "tar.gz is detected");

    const QString disguised =
    QDir(folder).filePath(QStringLiteral("archive.txt"));

    verify(
        QFile::copy(zip, disguised),
           "ZIP can be copied with a misleading extension");

    verify(
        thispcCanExtractArchive(QUrl::fromLocalFile(disguised), false),
           "archive detection uses content, not just extension");

    const QString fake =
    QDir(folder).filePath(QStringLiteral("fake.zip"));

    QFile fakeFile(fake);

    verify(
        fakeFile.open(QIODevice::WriteOnly),
           "fake ZIP file opens");

    verify(
        fakeFile.write("This is not an archive.\n") > 0,
           "fake ZIP file is written");

    fakeFile.close();

    verify(
        !thispcCanExtractArchive(QUrl::fromLocalFile(fake), false),
           "non-archive with ZIP extension is rejected");

    verify(
        !thispcCanExtractArchive(QUrl::fromLocalFile(folder), true),
           "directory is rejected");

    verify(
        !thispcCanExtractArchive(QUrl::fromLocalFile(zip), true),
           "directory flag prevents extraction");

    verify(
        !thispcCanExtractArchive(
            QUrl(QStringLiteral("sftp://example.test/archive.zip")),
                                 false),
                                 "remote URL is rejected");

    verify(
        !thispcCanExtractArchive(QUrl(), false),
           "invalid URL is rejected");

    verify(
        !thispcCanExtractArchive(
            QUrl::fromLocalFile(
                QDir(folder).filePath(QStringLiteral("missing.zip"))),
                                 false),
                                 "missing archive is rejected");

    std::printf(
        "PASS: %d archive detection assertions\n",
        checks);

    return 0;
}
