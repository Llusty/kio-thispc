/*
 * Safe, asynchronous Stage 1 file preview panel.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "archive-detection.h"

#include <QtConcurrentRun>

#include <exiv2/exiv2.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <archive.h>
#include <archive_entry.h>

#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QFutureWatcher>
#include <QHideEvent>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMimeDatabase>
#include <QPainter>
#include <QPdfDocument>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>
#include <cmath>

// Fit the cached image at paint time so the preview never clips during a resize.
// An asynchronous high-quality scale can replace the cached pixmap later without
// affecting the on-screen geometry.
class PreviewImageLabel final : public QLabel
{
public:
    explicit PreviewImageLabel(QWidget *parent = nullptr) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }

    // The scroll area must never size its child from QLabel's pixmap size.
    QSize sizeHint() const override { return QSize(0, 0); }
    QSize minimumSizeHint() const override { return QSize(0, 0); }

    void setSourceSize(const QSize &size)
    {
        m_sourceSize = size;
        update();
    }

    QRect fittedRect() const
    {
        if (pixmap().isNull() || m_sourceSize.isEmpty()) return {};
        const QRect area = contentsRect();
        if (area.isEmpty()) return {};
        QSize fitted = m_sourceSize;
        fitted.scale(area.size(), Qt::KeepAspectRatio);
        fitted = fitted.boundedTo(m_sourceSize); // Never enlarge a genuinely small image.
        return QRect(QPoint(area.x() + (area.width() - fitted.width()) / 2,
                            area.y() + (area.height() - fitted.height()) / 2), fitted);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QPixmap cached = pixmap();
        const QRect destination = fittedRect();
        if (cached.isNull() || destination.isEmpty()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(destination, cached, cached.rect());
    }

private:
    QSize m_sourceSize;
};

struct PreviewResult
{
    enum class Kind { Message, Text, Image, Folder, Metadata, Archive };
    Kind kind = Kind::Message;
    QString title;
    QString message;
    QString text;
    QImage image;
    QString metadata;
    qsizetype fileCount = 0;
    qsizetype directoryCount = 0;
    qsizetype skippedLinkCount = 0;
    qint64 directFileSize = 0;
    bool partial = false;
};

class PreviewPane : public QWidget
{
public:
    static constexpr qint64 TextLimit = 2 * 1024 * 1024;
    static constexpr qint64 ImageLimit = 50 * 1024 * 1024;
    static constexpr qint64 PdfLimit = 100 * 1024 * 1024;
    static constexpr qint64 MediaLimit = 4LL * 1024 * 1024 * 1024;
    static constexpr qsizetype MetadataTextLimit = 64 * 1024;
    static constexpr int PdfRenderEdgeLimit = 1600;
    static constexpr qint64 PdfRenderPixelLimit = 2'500'000;
    static constexpr qsizetype FolderEntryLimit = 10'000;
    static constexpr qint64 ArchiveInputLimit = 1024LL * 1024 * 1024;
    static constexpr qint64 ArchiveExpandedLimit = 8LL * 1024 * 1024 * 1024;
    static constexpr qsizetype ArchiveEntryLimit = 10'000;
    static constexpr qsizetype ArchiveListedEntryLimit = 500;
    static constexpr qsizetype ArchiveTextLimit = 64 * 1024;

    explicit PreviewPane(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("previewPane"));
        setMinimumWidth(260);
        setMaximumWidth(520);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 12);
        layout->setSpacing(8);

        m_title = new QLabel(this);
        QFont titleFont = m_title->font();
        titleFont.setBold(true);
        m_title->setFont(titleFont);
        m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_title->setWordWrap(true);
        layout->addWidget(m_title);

        m_stack = new QStackedWidget(this);
        m_message = new QLabel(m_stack);
        m_message->setAlignment(Qt::AlignCenter);
        m_message->setWordWrap(true);
        m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_stack->addWidget(m_message);

        m_text = new QPlainTextEdit(m_stack);
        m_text->setReadOnly(true);
        m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_stack->addWidget(m_text);

        m_imageScroll = new QScrollArea(m_stack);
        m_imageScroll->setWidgetResizable(true);
        m_imageScroll->setFrameShape(QFrame::NoFrame);
        m_imageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_imageScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_image = new PreviewImageLabel(m_imageScroll);
        m_image->setAlignment(Qt::AlignCenter);
        m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_imageScroll->setWidget(m_image);
        m_stack->addWidget(m_imageScroll);
        layout->addWidget(m_stack, 1);

        m_metadata = new QPlainTextEdit(this);
        m_metadata->setObjectName(QStringLiteral("previewMetadata"));
        m_metadata->setReadOnly(true);
        m_metadata->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_metadata->setMaximumHeight(150);
        m_metadata->hide();
        layout->addWidget(m_metadata);

        m_scaleTimer.setSingleShot(true);
        m_scaleTimer.setInterval(40);
        connect(&m_scaleTimer, &QTimer::timeout, this, &PreviewPane::startImageScale);

        showMessage(trLocal("Brak wybranego pliku", "No file selected"));
    }

    void setQuickLookLayout(bool enabled)
    {
        setMinimumWidth(enabled ? 0 : 260);
        setMaximumWidth(enabled ? QWIDGETSIZE_MAX : 520);
        m_metadata->setMaximumHeight(enabled ? 220 : 150);
    }

    void preview(const QUrl &url, bool isDirectory)
    {
        const quint64 request = ++m_request;
        if (!url.isValid()) {
            m_title->clear();
            showMessage(trLocal("Brak wybranego pliku", "No file selected"));
            return;
        }
        m_title->setText(url.fileName());
        if (!url.isLocalFile()) {
            showMessage(trLocal("Podgląd zdalnych plików nie jest obsługiwany.",
                                "Remote file preview is not supported."));
            return;
        }

        showMessage(trLocal("Wczytywanie podglądu…", "Loading preview…"));
        auto *watcher = new QFutureWatcher<PreviewResult>(this);
        connect(watcher, &QFutureWatcher<PreviewResult>::finished, this,
                [this, watcher, request] {
                    const PreviewResult result = watcher->result();
                    watcher->deleteLater();
                    if (request != m_request) return;
                    apply(result);
                });
        watcher->setFuture(QtConcurrent::run(
            isDirectory ? &PreviewPane::loadDirectory : &PreviewPane::load,
            url.toLocalFile()));
    }

protected:
    void hideEvent(QHideEvent *event) override
    {
        ++m_request;
        clearImage();
        m_metadata->hide();
        QWidget::hideEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (!m_originalImage.isNull()) scheduleImageScale();
    }

private:
    static PreviewResult loadDirectory(const QString &path)
    {
        PreviewResult result;
        const QFileInfo directoryInfo(path);
        result.title = directoryInfo.fileName();
        if (!directoryInfo.exists() || !directoryInfo.isDir() || !directoryInfo.isReadable()
            || directoryInfo.isSymbolicLink()) {
            result.message = trLocal("Nie można odczytać zawartości folderu.",
                                     "The folder contents cannot be read.");
            return result;
        }

        QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot,
                              QDirIterator::NoIteratorFlags);
        qsizetype visited = 0;
        while (visited < FolderEntryLimit && iterator.hasNext()) {
            const QFileInfo entry(iterator.next());
            ++visited;
            if (entry.isSymbolicLink()) {
                ++result.skippedLinkCount;
            } else if (entry.isDir()) {
                ++result.directoryCount;
            } else if (entry.isFile()) {
                ++result.fileCount;
                const qint64 size = entry.size();
                if (size > 0 && result.directFileSize <= std::numeric_limits<qint64>::max() - size) {
                    result.directFileSize += size;
                }
            }
        }
        result.partial = iterator.hasNext();
        result.kind = PreviewResult::Kind::Folder;
        return result;
    }

    static PreviewResult load(const QString &path)
    {
        PreviewResult result;
        const QFileInfo info(path);
        result.title = info.fileName();
        if (!info.exists() || !info.isFile() || !info.isReadable()) {
            result.message = trLocal("Nie można bezpiecznie odczytać pliku.",
                                     "The file cannot be read safely.");
            return result;
        }

        QMimeDatabase database;
        const QMimeType mime = database.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        const QString name = info.fileName().toLower();
        if (thispcIsArchiveCandidate(QUrl::fromLocalFile(path), false)) {
            return loadArchive(path, info);
        }
        if (mime.name().startsWith(QStringLiteral("audio/"))
            || mime.name().startsWith(QStringLiteral("video/"))) {
            return loadMedia(path, info, mime.name().startsWith(QStringLiteral("video/")));
        }
        if (mime.inherits(QStringLiteral("application/pdf"))
            || name.endsWith(QStringLiteral(".pdf"))) {
            return loadPdf(path, info);
        }
        const bool text = mime.name().startsWith(QStringLiteral("text/"))
            || mime.inherits(QStringLiteral("application/json"))
            || mime.inherits(QStringLiteral("application/xml"))
            || name.endsWith(QStringLiteral(".md"));
        if (text) {
            if (info.size() > TextLimit) {
                result.message = trLocal("Plik tekstowy jest zbyt duży do podglądu (limit 2 MiB).",
                                         "The text file is too large to preview (2 MiB limit).");
                return result;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                result.message = trLocal("Nie można odczytać pliku.", "The file cannot be read.");
                return result;
            }
            const QByteArray bytes = file.read(TextLimit + 1);
            if (bytes.contains('\0')) {
                result.message = trLocal("Plik zawiera dane binarne i nie może być pokazany jako tekst.",
                                         "The file contains binary data and cannot be shown as text.");
                return result;
            }
            result.kind = PreviewResult::Kind::Text;
            result.text = QString::fromUtf8(bytes);
            return result;
        }

        if (mime.name().startsWith(QStringLiteral("image/"))) {
            if (info.size() > ImageLimit) {
                result.message = trLocal("Obraz jest zbyt duży do podglądu (limit 50 MiB).",
                                         "The image is too large to preview (50 MiB limit).");
                return result;
            }
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QSize original = reader.size();
            if (original.isValid() && (original.width() > 12000 || original.height() > 12000
                || qint64(original.width()) * original.height() > 60'000'000)) {
                result.message = trLocal("Wymiary obrazu są zbyt duże do bezpiecznego podglądu.",
                                         "The image dimensions are too large for a safe preview.");
                return result;
            }
            result.image = reader.read();
            if (result.image.isNull()) {
                result.message = trLocal("Nie udało się odczytać obrazu.", "The image could not be decoded.");
                return result;
            }
            result.kind = PreviewResult::Kind::Image;
            result.metadata = loadExif(path);
            return result;
        }

        result.message = trLocal("Ten format nie jest jeszcze obsługiwany w podglądzie.",
                                 "This format is not yet supported by Preview.");
        return result;
    }

    static PreviewResult loadArchive(const QString &path, const QFileInfo &info)
    {
        PreviewResult result;
        result.title = info.fileName();
        if (info.size() > ArchiveInputLimit) {
            result.message = trLocal("Archiwum jest zbyt duże do podglądu (limit 1 GiB).",
                                     "The archive is too large to preview (1 GiB limit).");
            return result;
        }
        archive *reader = archive_read_new();
        if (!reader) {
            result.message = trLocal("Nie udało się utworzyć czytnika archiwum.",
                                     "The archive reader could not be created.");
            return result;
        }
        const auto release = qScopeGuard([&] { archive_read_free(reader); });
        archive_read_support_filter_none(reader);
        archive_read_support_filter_gzip(reader);
        archive_read_support_format_zip(reader);
        archive_read_support_format_7zip(reader);
        archive_read_support_format_tar(reader);
        if (archive_read_open_filename(reader, QFile::encodeName(path).constData(), 65536) != ARCHIVE_OK) {
            result.message = trLocal("Archiwum jest uszkodzone, zaszyfrowane lub nieobsługiwane.",
                                     "The archive is damaged, encrypted or unsupported.");
            return result;
        }
        QStringList lines;
        qsizetype entries = 0;
        qsizetype files = 0;
        qsizetype directories = 0;
        qint64 expanded = 0;
        archive_entry *entry = nullptr;
        int status = ARCHIVE_OK;
        while ((status = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
            if (++entries > ArchiveEntryLimit) {
                result.message = trLocal("Archiwum ma zbyt wiele wpisów do bezpiecznego podglądu (limit 10 000).",
                                         "The archive has too many entries to preview safely (10,000 limit).");
                return result;
            }
            const int format = archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK;
            if (format != ARCHIVE_FORMAT_ZIP && format != ARCHIVE_FORMAT_7ZIP
                && format != ARCHIVE_FORMAT_TAR) {
                result.message = trLocal("Format archiwum nie jest obsługiwany.", "The archive format is unsupported.");
                return result;
            }
            if (archive_entry_is_encrypted(entry) > 0) {
                result.message = trLocal("Archiwum jest zaszyfrowane i nie może być pokazane.",
                                         "The archive is encrypted and cannot be previewed.");
                return result;
            }
            const bool directory = archive_entry_filetype(entry) == AE_IFDIR;
            if (!directory && archive_entry_filetype(entry) != AE_IFREG) {
                result.message = trLocal("Archiwum zawiera nieobsługiwane lub niebezpieczne wpisy.",
                                         "The archive contains unsupported or unsafe entries.");
                return result;
            }
            const char *utf8 = archive_entry_pathname_utf8(entry);
            if (!utf8) {
                result.message = trLocal("Archiwum zawiera nieprawidłową nazwę wpisu.",
                                         "The archive contains an invalid entry name.");
                return result;
            }
            const QByteArray encoded(utf8);
            const QString raw = QString::fromUtf8(encoded);
            if (raw.toUtf8() != encoded) {
                result.message = trLocal("Archiwum zawiera nazwę inną niż UTF-8.",
                                         "The archive contains a non-UTF-8 entry name.");
                return result;
            }
            if (directory && (raw == QLatin1String(".") || raw == QLatin1String("./"))) continue;
            const QString safeName = thispcSafeArchiveEntryName(raw, directory);
            if (safeName.isEmpty()) {
                result.message = trLocal("Archiwum zawiera niebezpieczną ścieżkę.",
                                         "The archive contains an unsafe path.");
                return result;
            }
            const qint64 size = directory ? 0 : archive_entry_size(entry);
            if (size < 0 || (!directory && expanded > ArchiveExpandedLimit - size)) {
                result.message = trLocal("Deklarowany rozmiar archiwum przekracza limit 8 GiB.",
                                         "The archive's declared size exceeds the 8 GiB limit.");
                return result;
            }
            expanded += size;
            directory ? ++directories : ++files;
            if (lines.size() < ArchiveListedEntryLimit) {
                lines.push_back((directory ? QStringLiteral("[KATALOG] ") : QStringLiteral("[PLIK] "))
                    + safeName + (directory ? QString() : QStringLiteral(" — ") + formatFileSize(size, false)));
            }
            if (archive_read_data_skip(reader) != ARCHIVE_OK) {
                result.message = trLocal("Nie udało się bezpiecznie pominąć danych wpisu archiwum.",
                                         "Archive entry data could not be skipped safely.");
                return result;
            }
        }
        if (status != ARCHIVE_EOF || archive_read_close(reader) != ARCHIVE_OK) {
            result.message = trLocal("Archiwum jest uszkodzone lub niekompletne.",
                                     "The archive is damaged or incomplete.");
            return result;
        }
        if (entries == 0) {
            result.message = trLocal("Archiwum jest puste.", "The archive is empty.");
            return result;
        }
        QStringList summary = {
            trLocal("Katalogi: ", "Directories: ") + QString::number(directories),
            trLocal("Pliki: ", "Files: ") + QString::number(files),
            trLocal("Łączny deklarowany rozmiar: ", "Total declared size: ")
                + formatFileSize(expanded, false), QString()
        };
        summary.append(lines);
        if (entries > ArchiveListedEntryLimit) {
            summary.push_back(trLocal("… pokazano pierwsze 500 z %1 wpisów.",
                                      "… showing the first 500 of %1 entries.").arg(entries));
        }
        result.kind = PreviewResult::Kind::Archive;
        result.text = summary.join(QLatin1Char('\n')).left(ArchiveTextLimit);
        return result;
    }

    static PreviewResult loadPdf(const QString &path, const QFileInfo &info)
    {
        PreviewResult result;
        result.title = info.fileName();
        if (info.size() > PdfLimit) {
            result.message = trLocal("Dokument PDF jest zbyt duży do podglądu (limit 100 MiB).",
                                     "The PDF is too large to preview (100 MiB limit).");
            return result;
        }

        // QPdfDocument is created, used and destroyed in this worker thread.
        // No QObject or PDFium state crosses the thread boundary.
        QPdfDocument document;
        const QPdfDocument::Error loadError = document.load(path);
        if (loadError != QPdfDocument::Error::None
            || document.status() != QPdfDocument::Status::Ready) {
            if (loadError == QPdfDocument::Error::IncorrectPassword
                || loadError == QPdfDocument::Error::UnsupportedSecurityScheme) {
                result.message = trLocal("Dokument PDF jest zaszyfrowany i nie można go wyświetlić.",
                                         "The PDF is encrypted and cannot be previewed.");
            } else {
                result.message = trLocal("Nie udało się odczytać dokumentu PDF.",
                                         "The PDF could not be read.");
            }
            return result;
        }
        if (document.pageCount() < 1) {
            result.message = trLocal("Dokument PDF nie zawiera stron.",
                                     "The PDF contains no pages.");
            return result;
        }

        const QSizeF points = document.pagePointSize(0);
        if (!points.isValid() || points.isEmpty()) {
            result.message = trLocal("Pierwsza strona PDF ma nieprawidłowy rozmiar.",
                                     "The first PDF page has an invalid size.");
            return result;
        }
        QSize renderSize = points.toSize();
        renderSize.scale(QSize(PdfRenderEdgeLimit, PdfRenderEdgeLimit), Qt::KeepAspectRatio);
        if (qint64(renderSize.width()) * renderSize.height() > PdfRenderPixelLimit) {
            const double scale = std::sqrt(double(PdfRenderPixelLimit)
                                           / (double(renderSize.width()) * renderSize.height()));
            renderSize = QSize(qMax(1, int(renderSize.width() * scale)),
                               qMax(1, int(renderSize.height() * scale)));
        }
        const QImage rendered = document.render(0, renderSize);
        if (rendered.isNull()) {
            result.message = trLocal("Nie udało się wyrenderować pierwszej strony PDF.",
                                     "The first PDF page could not be rendered.");
            return result;
        }
        // PDF pages are opaque sheets of paper, while QtPdf returns an ARGB
        // image whose untouched page areas can remain transparent.  Letting
        // those pixels reach a QLabel makes the widget/palette background show
        // through (typically grey in Plasma), so blank or partly blank pages
        // look grey. Composite in the worker, before the image crosses threads.
        result.image = QImage(rendered.size(), QImage::Format_RGB32);
        result.image.fill(Qt::white);
        {
            QPainter painter(&result.image);
            painter.drawImage(QPoint(), rendered);
        }
        result.kind = PreviewResult::Kind::Image;
        return result;
    }

    static QString tagString(const TagLib::String &value)
    {
        const std::string utf8 = value.to8Bit(true);
        return QString::fromUtf8(utf8.data(), qsizetype(utf8.size())).trimmed();
    }

    static void addMetadataLine(QStringList &lines, const QString &label, QString value)
    {
        value = value.trimmed();
        value.replace(QLatin1Char('\n'), QLatin1Char(' '));
        value.replace(QLatin1Char('\r'), QLatin1Char(' '));
        if (!value.isEmpty()) lines.push_back(label + QStringLiteral(": ") + value.left(1024));
    }

    static PreviewResult loadMedia(const QString &path, const QFileInfo &info, bool video)
    {
        PreviewResult result;
        result.title = info.fileName();
        if (info.size() > MediaLimit) {
            result.message = trLocal("Plik multimedialny jest zbyt duży do analizy (limit 4 GiB).",
                                     "The media file is too large to inspect (4 GiB limit).");
            return result;
        }
        const QByteArray encoded = QFile::encodeName(path);
        TagLib::FileRef file(encoded.constData(), true, TagLib::AudioProperties::Fast);
        if (file.isNull() || !file.audioProperties()) {
            result.message = trLocal("Nie udało się odczytać metadanych multimediów.",
                                     "The media metadata could not be read.");
            return result;
        }

        QStringList lines;
        lines << (video ? trLocal("Typ: wideo", "Type: Video")
                        : trLocal("Typ: audio", "Type: Audio"));
        if (const TagLib::Tag *tag = file.tag()) {
            addMetadataLine(lines, trLocal("Tytuł", "Title"), tagString(tag->title()));
            addMetadataLine(lines, trLocal("Wykonawca", "Artist"), tagString(tag->artist()));
            addMetadataLine(lines, trLocal("Album", "Album"), tagString(tag->album()));
            if (tag->year() > 0) addMetadataLine(lines, trLocal("Rok", "Year"), QString::number(tag->year()));
            if (tag->track() > 0) addMetadataLine(lines, trLocal("Ścieżka", "Track"), QString::number(tag->track()));
        }
        const TagLib::AudioProperties *properties = file.audioProperties();
        const qint64 milliseconds = properties->lengthInMilliseconds();
        if (milliseconds >= 0) {
            const qint64 seconds = milliseconds / 1000;
            addMetadataLine(lines, trLocal("Czas", "Duration"),
                            QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
        }
        if (properties->bitrate() > 0)
            addMetadataLine(lines, trLocal("Przepływność", "Bitrate"),
                            QString::number(properties->bitrate()) + QStringLiteral(" kb/s"));
        if (properties->sampleRate() > 0)
            addMetadataLine(lines, trLocal("Próbkowanie", "Sample rate"),
                            QString::number(properties->sampleRate()) + QStringLiteral(" Hz"));
        if (properties->channels() > 0)
            addMetadataLine(lines, trLocal("Kanały", "Channels"), QString::number(properties->channels()));
        result.kind = PreviewResult::Kind::Metadata;
        result.text = lines.join(QLatin1Char('\n')).left(MetadataTextLimit);
        return result;
    }

    static QString loadExif(const QString &path)
    {
        try {
            auto image = Exiv2::ImageFactory::open(path.toStdString());
            if (!image.get()) return {};
            image->readMetadata();
            const Exiv2::ExifData &data = image->exifData();
            QStringList lines;
            const auto append = [&](const char *key, const QString &label) {
                const auto item = data.findKey(Exiv2::ExifKey(key));
                if (item != data.end()) addMetadataLine(lines, label, QString::fromStdString(item->toString()));
            };
            append("Exif.Image.Make", trLocal("Aparat", "Camera make"));
            append("Exif.Image.Model", trLocal("Model", "Camera model"));
            append("Exif.Photo.DateTimeOriginal", trLocal("Data wykonania", "Date taken"));
            append("Exif.Photo.ExposureTime", trLocal("Czas ekspozycji", "Exposure"));
            append("Exif.Photo.FNumber", trLocal("Przysłona", "Aperture"));
            append("Exif.Photo.ISOSpeedRatings", QStringLiteral("ISO"));
            append("Exif.Photo.FocalLength", trLocal("Ogniskowa", "Focal length"));
            return lines.join(QLatin1Char('\n')).left(MetadataTextLimit);
        } catch (const Exiv2::Error &) {
            return {};
        } catch (...) {
            return {};
        }
    }

    void showMessage(const QString &message)
    {
        clearImage();
        m_metadata->hide();
        m_message->setText(message);
        m_stack->setCurrentWidget(m_message);
    }

    void clearImage()
    {
        ++m_scaleRequest;
        m_scaleTimer.stop();
        m_originalImage = QImage();
        m_image->clear();
        m_image->setSourceSize({});
    }

    void scheduleImageScale()
    {
        ++m_scaleRequest;
        m_scaleTimer.start();
    }

    void startImageScale()
    {
        if (m_originalImage.isNull()) return;
        const QSize viewport = m_imageScroll->viewport()->size();
        if (!viewport.isValid() || viewport.isEmpty()) return;

        const QSize target = m_originalImage.size().boundedTo(viewport);
        const quint64 request = m_scaleRequest;
        const QImage original = m_originalImage;
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this,
                [this, watcher, request] {
                    const QImage scaled = watcher->result();
                    watcher->deleteLater();
                    if (request != m_scaleRequest || scaled.isNull()) return;
                    m_image->setPixmap(QPixmap::fromImage(scaled));
                });
        watcher->setFuture(QtConcurrent::run([original, target] {
            if (target == original.size()) return original;
            return original.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }));
    }

    void apply(const PreviewResult &result)
    {
        if (!result.title.isEmpty()) m_title->setText(result.title);
        if (result.kind == PreviewResult::Kind::Text) {
            clearImage();
            m_metadata->hide();
            m_text->setPlainText(result.text);
            m_text->moveCursor(QTextCursor::Start);
            m_stack->setCurrentWidget(m_text);
        } else if (result.kind == PreviewResult::Kind::Image) {
            m_originalImage = result.image;
            m_image->setSourceSize(m_originalImage.size());
            m_stack->setCurrentWidget(m_imageScroll);
            m_metadata->setPlainText(result.metadata);
            m_metadata->setVisible(!result.metadata.isEmpty());
            scheduleImageScale();
        } else if (result.kind == PreviewResult::Kind::Metadata
                   || result.kind == PreviewResult::Kind::Archive) {
            clearImage();
            m_metadata->hide();
            m_text->setPlainText(result.text);
            m_text->moveCursor(QTextCursor::Start);
            m_stack->setCurrentWidget(m_text);
        } else if (result.kind == PreviewResult::Kind::Folder) {
            QStringList lines = {
                trLocal("Podfoldery: ", "Subfolders: ") + QString::number(result.directoryCount),
                trLocal("Pliki: ", "Files: ") + QString::number(result.fileCount),
                trLocal("Rozmiar plików bezpośrednio w folderze: ",
                        "Size of files directly in this folder: ")
                    + formatFileSize(result.directFileSize, false)
            };
            if (result.skippedLinkCount > 0) {
                lines.push_back(trLocal("Pominięte dowiązania symboliczne: ",
                                        "Skipped symbolic links: ")
                                + QString::number(result.skippedLinkCount));
            }
            if (result.partial) {
                lines.push_back(trLocal(
                    "Wyniki są niepełne: sprawdzono pierwsze 10 000 wpisów.",
                    "Results are incomplete: only the first 10,000 entries were checked."));
            }
            showMessage(lines.join(QLatin1Char('\n')));
        } else {
            showMessage(result.message);
        }
    }

    quint64 m_request = 0;
    QLabel *m_title = nullptr;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_message = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QScrollArea *m_imageScroll = nullptr;
    PreviewImageLabel *m_image = nullptr;
    QPlainTextEdit *m_metadata = nullptr;
    QImage m_originalImage;
    QTimer m_scaleTimer;
    quint64 m_scaleRequest = 0;
};
