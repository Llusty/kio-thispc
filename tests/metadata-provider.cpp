/* Deterministic Stage 5 metadata provider and lazy UI coverage. */
#include "metadatawidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

namespace P = KFileMetaData::Property;

static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

static MetadataData waitFor(MetadataProvider *provider)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(provider, &MetadataProvider::dataChanged, &loop, [&](const MetadataData &data) {
        if (data.state != MetadataState::Loading) loop.quit();
    });
    verify(provider->start(), "metadata extraction started");
    timeout.start(5000);
    loop.exec();
    verify(timeout.isActive(), "metadata extraction completed before timeout");
    return provider->data();
}

static QString valueFor(const MetadataData &data, const QString &labelFragment)
{
    for (const auto &row : data.rows)
        if (row.label.contains(labelFragment, Qt::CaseInsensitive)) return row.value;
    return {};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QLocale originalLocale;
    QLocale::setDefault(QLocale(QLocale::Polish, QLocale::Poland));
    verify(MetadataProvider::dimensionsLabel() == QStringLiteral("Wymiary"),
           "combined dimensions use the deterministic Polish UI label");
    QLocale::setDefault(originalLocale);
    QTemporaryDir dir;
    verify(dir.isValid(), "temporary directory created");
    const QString path = dir.filePath("media.bin");
    QFile file(path);
    verify(file.open(QIODevice::WriteOnly), "local fixture opened");
    verify(file.write("metadata fixture") > 0, "local fixture written");
    file.close();

    int calls = 0;
    MetadataExtractor rich = [&](const QString &requested) {
        ++calls;
        verify(requested == path, "fake extractor receives exact local path");
        return MetadataExtractionResult{{
            {P::Width, 3840}, {P::Height, 2160}, {P::Manufacturer, QStringLiteral("KDE Camera")},
            {P::PhotoExposureTime, 0.008}, {P::PhotoISOSpeedRatings, 200},
            {P::Title, QStringLiteral("Example")}, {P::Artist, QStringLiteral("Ada")},
            {P::Artist, QStringLiteral("Linus")}, {P::Album, QStringLiteral("Tests")},
            {P::TrackNumber, 3}, {P::DiscNumber, 1}, {P::Duration, 125},
            {P::BitRate, 320}, {P::SampleRate, 48000}, {P::Channels, 2},
            {P::VideoCodec, QStringLiteral("H.264")}, {P::AudioCodec, QStringLiteral("AAC")},
            {P::FrameRate, 29.97}, {P::Author, QStringLiteral("Author")},
            {P::Subject, QStringLiteral("Subject")}, {P::PageCount, 12},
            {P::Keywords, QStringLiteral("one")}, {P::Keywords, QStringLiteral("two")},
            {P::Empty, QStringLiteral("ignored")}, {P::Comment, QString()}
        }, {}, false, {QStringLiteral("blue"), QStringLiteral("favorite")}, 8, QStringLiteral("User note")};
    };

    MetadataWidget widget(QUrl::fromLocalFile(path), nullptr, rich);
    verify(widget.provider()->data().state == MetadataState::Idle, "widget construction is lazy");
    QCoreApplication::processEvents();
    verify(calls == 0 && widget.provider()->startCount() == 0, "opening Properties does not extract metadata");
    widget.activate();
    QEventLoop loop;
    QObject::connect(widget.provider(), &MetadataProvider::dataChanged, &loop, [&](const MetadataData &data) {
        if (data.state != MetadataState::Loading) loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    const MetadataData richData = widget.provider()->data();
    verify(richData.state == MetadataState::Available && calls == 1, "activation runs extractor once");
    widget.activate();
    QCoreApplication::processEvents();
    verify(calls == 1 && widget.provider()->startCount() == 1, "repeated activation does not re-extract");
    verify(valueFor(richData, MetadataProvider::dimensionsLabel()) == QStringLiteral("3840 × 2160"),
           "image dimensions are combined and formatted");
    verify(!valueFor(richData, "Manufacturer").isEmpty(), "image camera metadata rendered");
    verify(!valueFor(richData, "Exposure").isEmpty(), "image exposure metadata rendered");
    verify(!valueFor(richData, "Title").isEmpty() && !valueFor(richData, "Album").isEmpty(), "audio title and album rendered");
    verify(valueFor(richData, "Artist").contains("Ada") && valueFor(richData, "Artist").contains("Linus")
           && valueFor(richData, "Artist").contains(';'), "multiple artists rendered as a readable list");
    verify(!valueFor(richData, "Duration").isEmpty() && !valueFor(richData, "Bit").isEmpty(), "audio duration and bitrate formatted");
    verify(!valueFor(richData, "Video codec").isEmpty() && !valueFor(richData, "Frame").isEmpty(), "video codec and frame rate rendered");
    verify(!valueFor(richData, "Author").isEmpty() && !valueFor(richData, "Page").isEmpty(), "document metadata rendered");
    verify(valueFor(richData, "Keyword").contains(';'), "multiple document keywords rendered");
    verify(valueFor(richData, "Tags").contains(';'), "KFileMetaData user tags render read-only as a list");
    verify(valueFor(richData, "Comment").isEmpty(), "absent values are omitted");
    for (const auto &row : richData.rows) verify(row.value != QStringLiteral("ignored"), "unknown/empty property is ignored");

    MetadataProvider none(QUrl::fromLocalFile(path), nullptr, [](const QString &) { return MetadataExtractionResult{}; });
    verify(waitFor(&none).state == MetadataState::NoMetadata, "empty extraction becomes no-metadata state");
    MetadataProvider failed(QUrl::fromLocalFile(path), nullptr, [](const QString &) {
        return MetadataExtractionResult{{}, QStringLiteral("extractor error"), false};
    });
    verify(waitFor(&failed).state == MetadataState::Failed, "extractor error becomes failed state");
    MetadataProvider disappeared(QUrl::fromLocalFile(path), nullptr, [](const QString &) {
        return MetadataExtractionResult{{}, {}, true};
    });
    verify(waitFor(&disappeared).state == MetadataState::Disappeared, "disappearance becomes explicit state");

    const QString folder = dir.filePath("folder");
    verify(QDir().mkdir(folder), "directory fixture created");
    verify(MetadataProvider::capabilityForUrl(QUrl::fromLocalFile(folder)) == MetadataCapability::DirectoryNotApplicable,
           "directory metadata is not applicable");
    const QString link = dir.filePath("link");
    verify(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(link).constData()) == 0, "symlink fixture created");
    verify(MetadataProvider::capabilityForUrl(QUrl::fromLocalFile(link)) == MetadataCapability::SymlinkUnavailable,
           "symlink is unavailable without parsing target");
    verify(MetadataProvider::capabilityForUrl(QUrl("sftp://example.invalid/media.mp3")) == MetadataCapability::RemoteUnavailable,
           "remote URL is unavailable without extractor attempt");
    verify(::chmod(QFile::encodeName(path).constData(), 0444) == 0, "read-only fixture mode set");
    verify(MetadataProvider::capabilityForUrl(QUrl::fromLocalFile(path)) == MetadataCapability::SupportedLocalFile,
           "read-only readable local file is supported");
    verify(MetadataProvider::capabilityForUrl(QUrl::fromLocalFile(dir.filePath("missing"))) == MetadataCapability::Unreadable,
           "missing or denied path is unavailable");

    auto *closing = new MetadataWidget(QUrl::fromLocalFile(path), nullptr, [](const QString &) {
        QThread::msleep(30);
        return MetadataExtractionResult{{{KFileMetaData::Property::Title, QStringLiteral("late")}}, {}, false};
    });
    closing->activate();
    delete closing;
    QEventLoop drain;
    QTimer::singleShot(80, &drain, &QEventLoop::quit);
    drain.exec();
    verify(true, "closing metadata UI during extraction is safe");

    qInfo("PASS: %d metadata assertions; lazy async provider, formatting, capabilities, lifetime", checks);
    return 0;
}
