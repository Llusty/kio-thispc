static int checks = 0;
static void verify(bool value, const char *description)
{
    if (!value) qFatal("FAIL: %s", description);
    ++checks;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("thispc-preview-test"));
    QCoreApplication::setApplicationName(QStringLiteral("preview-test"));
    QSettings().clear();
    QTemporaryDir files;
    verify(files.isValid(), "disposable preview directory");

    QFile text(files.filePath("sample.json"));
    verify(text.open(QIODevice::WriteOnly), "create text fixture");
    text.write("{\"safe\": true}");
    text.close();

    const auto createImage = [&](const QString &name, const QSize &size, const QColor &color) {
        QImage image(size, QImage::Format_ARGB32);
        image.fill(color);
        verify(image.save(files.filePath(name)), "create image fixture");
    };
    createImage(QStringLiteral("wide.png"), QSize(1600, 400), Qt::red);
    createImage(QStringLiteral("tall.png"), QSize(400, 1600), Qt::green);
    createImage(QStringLiteral("small.png"), QSize(24, 18), Qt::blue);
    createImage(QStringLiteral("photo.jpg"), QSize(80, 60), Qt::yellow);
    {
        auto image = Exiv2::ImageFactory::open(files.filePath("photo.jpg").toStdString());
        verify(image.get() != nullptr, "open deterministic EXIF fixture");
        image->readMetadata();
        Exiv2::ExifData &exif = image->exifData();
        exif["Exif.Image.Make"] = "Test Camera Company";
        exif["Exif.Image.Model"] = "Deterministic Model";
        exif["Exif.Photo.DateTimeOriginal"] = "2026:09:18 12:34:56";
        exif["Exif.Photo.ISOSpeedRatings"] = uint16_t(200);
        image->writeMetadata();
    }

    const QString wavPath = files.filePath(QStringLiteral("tone.wav"));
    {
        QFile wav(wavPath);
        verify(wav.open(QIODevice::WriteOnly), "create deterministic WAV fixture");
        const quint32 sampleRate = 8000;
        const quint32 samples = sampleRate;
        const quint32 dataSize = samples * 2;
        QDataStream stream(&wav);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + dataSize);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
               << quint32(sampleRate * 2) << quint16(2) << quint16(16);
        stream.writeRawData("data", 4);
        stream << dataSize;
        for (quint32 index = 0; index < samples; ++index) stream << qint16(0);
    }

    QFile brokenVideo(files.filePath(QStringLiteral("broken.mp4")));
    verify(brokenVideo.open(QIODevice::WriteOnly), "create broken video fixture");
    brokenVideo.write("not a media container");
    brokenVideo.close();

    const auto createZip = [&](const QString &path,
                               const QList<QPair<QString, QByteArray>> &entries) {
        archive *writer = archive_write_new();
        verify(writer != nullptr, "create ZIP writer");
        verify(archive_write_set_format_zip(writer) == ARCHIVE_OK, "select ZIP format");
        verify(archive_write_open_filename(writer, QFile::encodeName(path).constData()) == ARCHIVE_OK,
               "open ZIP fixture");
        for (const auto &[name, data] : entries) {
            archive_entry *entry = archive_entry_new();
            const QByteArray encoded = name.toUtf8();
            archive_entry_set_pathname_utf8(entry, encoded.constData());
            if (name.endsWith(QLatin1Char('/'))) {
                archive_entry_set_filetype(entry, AE_IFDIR);
                archive_entry_set_perm(entry, 0755);
                archive_entry_set_size(entry, 0);
            } else {
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);
                archive_entry_set_size(entry, data.size());
            }
            verify(archive_write_header(writer, entry) == ARCHIVE_OK, "write ZIP entry header");
            if (!data.isEmpty())
                verify(archive_write_data(writer, data.constData(), data.size()) == data.size(),
                       "write ZIP entry data");
            archive_entry_free(entry);
        }
        verify(archive_write_close(writer) == ARCHIVE_OK, "close ZIP fixture");
        archive_write_free(writer);
    };
    const QString archivePath = files.filePath(QStringLiteral("sample.zip"));
    createZip(archivePath, {{QStringLiteral("dokumenty/"), {}},
                            {QStringLiteral("dokumenty/żółw.txt"), QByteArray("safe contents")},
                            {QStringLiteral("root.bin"), QByteArray("12345")}});
    const QString emptyArchivePath = files.filePath(QStringLiteral("empty.zip"));
    createZip(emptyArchivePath, {});
    QFile brokenArchive(files.filePath(QStringLiteral("broken.zip")));
    verify(brokenArchive.open(QIODevice::WriteOnly), "create broken archive fixture");
    brokenArchive.write("PK\003\004truncated archive");
    brokenArchive.close();

    const QString excessiveArchivePath = files.filePath(QStringLiteral("excessive.zip"));
    {
        archive *writer = archive_write_new();
        verify(writer && archive_write_set_format_zip(writer) == ARCHIVE_OK
                   && archive_write_open_filename(writer, QFile::encodeName(excessiveArchivePath).constData()) == ARCHIVE_OK,
               "create excessive ZIP fixture");
        for (qsizetype index = 0; index <= PreviewPane::ArchiveEntryLimit; ++index) {
            archive_entry *entry = archive_entry_new();
            const QByteArray name = QByteArray("entry-") + QByteArray::number(index);
            archive_entry_set_pathname(entry, name.constData());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0600);
            archive_entry_set_size(entry, 0);
            if (archive_write_header(writer, entry) != ARCHIVE_OK)
                qFatal("FAIL: write excessive ZIP entry");
            archive_entry_free(entry);
        }
        verify(archive_write_close(writer) == ARCHIVE_OK, "close excessive ZIP fixture");
        archive_write_free(writer);
    }

    const QString pdfPath = files.filePath(QStringLiteral("sample.pdf"));
    {
        QPdfWriter writer(pdfPath);
        writer.setPageSize(QPageSize(QSizeF(200, 100), QPageSize::Point));
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.fillRect(QRect(0, 0, 200, 100), QColor(245, 245, 245));
        painter.setPen(Qt::black);
        painter.drawText(QRect(10, 10, 180, 80), Qt::AlignCenter, QStringLiteral("PDF preview fixture"));
    }
    verify(QFileInfo(pdfPath).size() > 0, "create deterministic PDF fixture");

    const QString blankPdfPath = files.filePath(QStringLiteral("blank.pdf"));
    {
        QPdfWriter writer(blankPdfPath);
        writer.setPageSize(QPageSize(QSizeF(180, 120), QPageSize::Point));
        writer.setResolution(72);
        QPainter painter(&writer); // An intentionally untouched, transparent render surface.
    }
    verify(QFileInfo(blankPdfPath).size() > 0, "create blank PDF fixture");

    const QString mixedPdfPath = files.filePath(QStringLiteral("mixed-content.pdf"));
    {
        QPdfWriter writer(mixedPdfPath);
        writer.setPageSize(QPageSize(QSizeF(240, 160), QPageSize::Point));
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.fillRect(QRect(20, 20, 80, 60), QColor(20, 70, 160));
        QImage scan(QSize(60, 40), QImage::Format_RGB32);
        scan.fill(QColor(210, 180, 80));
        painter.drawImage(QRect(140, 90, 60, 40), scan);
    }
    verify(QFileInfo(mixedPdfPath).size() > 0, "create mixed vector and scan PDF fixture");

    QFile brokenPdf(files.filePath(QStringLiteral("broken.pdf")));
    verify(brokenPdf.open(QIODevice::WriteOnly), "create broken PDF fixture");
    brokenPdf.write("%PDF-1.7\nthis is deliberately not a PDF\n%%EOF\n");
    brokenPdf.close();

    QFile binary(files.filePath("sample.bin"));
    verify(binary.open(QIODevice::WriteOnly), "create unsupported fixture");
    binary.write(QByteArray("a\0b", 3));
    binary.close();

    const QString folderPath = files.filePath(QStringLiteral("folder"));
    const QString childPath = QDir(folderPath).filePath(QStringLiteral("child"));
    verify(QDir().mkpath(childPath), "create folder preview fixture");
    QFile directOne(QDir(folderPath).filePath(QStringLiteral("one.bin")));
    QFile directTwo(QDir(folderPath).filePath(QStringLiteral("two.bin")));
    QFile nested(QDir(childPath).filePath(QStringLiteral("nested.bin")));
    verify(directOne.open(QIODevice::WriteOnly) && directTwo.open(QIODevice::WriteOnly)
               && nested.open(QIODevice::WriteOnly),
           "create direct and nested folder files");
    directOne.write("1234");
    directTwo.write("12345");
    nested.write("this size must not be counted");
    directOne.close();
    directTwo.close();
    nested.close();
    verify(QFile::link(directOne.fileName(),
                       QDir(folderPath).filePath(QStringLiteral("ignored-link"))),
           "create skipped symbolic link fixture");
    const QString linkedFolderPath = QDir(folderPath).filePath(QStringLiteral("linked-child"));
    verify(QFile::link(childPath, linkedFolderPath),
           "create skipped folder symbolic link fixture");

    PreviewPane pane;
    pane.resize(340, 500);
    pane.show();

    pane.preview(QUrl::fromLocalFile(text.fileName()), false);
    verify(QTest::qWaitFor([&] { return pane.m_stack->currentWidget() == pane.m_text; }, 3000),
           "JSON preview completes asynchronously");
    verify(pane.m_text->toPlainText() == QStringLiteral("{\"safe\": true}"),
           "JSON is rendered as inert plain text");

    const auto waitForImageSize = [&](const QSize &original) {
        return QTest::qWaitFor([&] {
            if (pane.m_stack->currentWidget() != pane.m_imageScroll
                || pane.m_image->pixmap().isNull()) return false;
            const QSize rendered = pane.m_image->pixmap().size();
            const QSize viewport = pane.m_imageScroll->viewport()->size();
            return rendered.width() <= viewport.width() && rendered.height() <= viewport.height()
                && qAbs(double(rendered.width()) / rendered.height()
                        - double(original.width()) / original.height()) < 0.02;
        }, 3000);
    };

    pane.preview(QUrl::fromLocalFile(files.filePath("wide.png")), false);
    verify(QTest::qWaitFor([&] { return pane.m_stack->currentWidget() == pane.m_imageScroll; }, 3000),
           "image preview completes asynchronously");
    verify(waitForImageSize(QSize(1600, 400)), "wide image fits the visible viewport without cropping");

    pane.preview(QUrl::fromLocalFile(files.filePath("tall.png")), false);
    verify(waitForImageSize(QSize(400, 1600)), "tall image fits the visible viewport without cropping");

    pane.preview(QUrl::fromLocalFile(files.filePath("small.png")), false);
    verify(QTest::qWaitFor([&] { return pane.m_image->pixmap().size() == QSize(24, 18); }, 3000),
           "small image is not upscaled");

    pane.preview(QUrl::fromLocalFile(files.filePath("photo.jpg")), false);
    verify(waitForImageSize(QSize(80, 60)), "EXIF image remains a visual preview");
    verify(QTest::qWaitFor([&] {
               const QString metadata = pane.m_metadata->toPlainText();
               return pane.m_metadata->isVisible()
                   && metadata.contains("Camera make: Test Camera Company")
                   && metadata.contains("Camera model: Deterministic Model")
                   && metadata.contains("Date taken: 2026:09:18 12:34:56")
                   && metadata.contains("ISO: 200");
           }, 3000), "selected EXIF fields are shown below the image");

    pane.preview(QUrl::fromLocalFile(wavPath), false);
    verify(QTest::qWaitFor([&] {
               const QString metadata = pane.m_text->toPlainText();
               return pane.m_stack->currentWidget() == pane.m_text
                   && metadata.contains("Type: Audio")
                   && metadata.contains("Duration: 0:01")
                   && metadata.contains("Sample rate: 8000 Hz")
                   && metadata.contains("Channels: 1");
           }, 3000), "audio properties are read without playback");
    verify(!pane.m_metadata->isVisible(), "standalone media metadata hides the EXIF area");

    pane.preview(QUrl::fromLocalFile(archivePath), false);
    verify(QTest::qWaitFor([&] {
               const QString manifest = pane.m_text->toPlainText();
               return pane.m_stack->currentWidget() == pane.m_text
                   && manifest.contains("Directories: 1")
                   && manifest.contains("Files: 2")
                   && manifest.contains("[KATALOG] dokumenty")
                   && manifest.contains(QStringLiteral("[PLIK] dokumenty/żółw.txt — 13 B"))
                   && manifest.contains("[PLIK] root.bin — 5 B");
           }, 3000), "archive manifest shows ordered directory, UTF-8 file names and sizes");
    verify(!pane.m_metadata->isVisible(), "archive manifest does not reuse the EXIF area");

    const PreviewResult emptyArchive = PreviewPane::loadArchive(emptyArchivePath, QFileInfo(emptyArchivePath));
    verify(emptyArchive.kind == PreviewResult::Kind::Message
               && emptyArchive.message.contains("empty"),
           "empty archive has an explicit message");
    const PreviewResult invalidArchive = PreviewPane::loadArchive(
        brokenArchive.fileName(), QFileInfo(brokenArchive.fileName()));
    verify(invalidArchive.kind == PreviewResult::Kind::Message
               && (invalidArchive.message.contains("damaged")
                   || invalidArchive.message.contains("incomplete")),
           "broken archive has a safe error");
    const PreviewResult excessiveArchive = PreviewPane::loadArchive(
        excessiveArchivePath, QFileInfo(excessiveArchivePath));
    verify(excessiveArchive.kind == PreviewResult::Kind::Message
               && excessiveArchive.message.contains("too many entries"),
           "archive entry limit stops manifest enumeration");

    pane.preview(QUrl::fromLocalFile(excessiveArchivePath), false);
    pane.preview(QUrl::fromLocalFile(text.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_text
                   && pane.m_text->toPlainText() == QStringLiteral("{\"safe\": true}");
           }, 3000), "late archive result cannot replace a newer selection");
    pane.preview(QUrl::fromLocalFile(excessiveArchivePath), false);
    pane.hide();
    QTest::qWait(100);
    verify(pane.m_originalImage.isNull() && pane.m_image->pixmap().isNull(),
           "hiding Preview invalidates an in-flight archive result");
    pane.show();

    const PreviewResult videoMetadata = PreviewPane::loadMedia(
        wavPath, QFileInfo(wavPath), true);
    verify(videoMetadata.kind == PreviewResult::Kind::Metadata
               && videoMetadata.text.contains("Type: Video")
               && videoMetadata.text.contains("Duration: 0:01"),
           "video containers use the same bounded metadata path without playback");

    pane.preview(QUrl::fromLocalFile(brokenVideo.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_message
                   && pane.m_message->text().contains("could not be read");
           }, 3000), "broken video has a safe metadata error");

    pane.preview(QUrl::fromLocalFile(wavPath), false);
    pane.preview(QUrl::fromLocalFile(text.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_text
                   && pane.m_text->toPlainText() == QStringLiteral("{\"safe\": true}");
           }, 3000), "late media metadata cannot replace a newer selection");

    pane.preview(QUrl::fromLocalFile(pdfPath), false);
    verify(waitForImageSize(QSize(200, 100)), "first PDF page is rendered proportionally");
    verify(pane.m_originalImage.width() <= PreviewPane::PdfRenderEdgeLimit
               && pane.m_originalImage.height() <= PreviewPane::PdfRenderEdgeLimit
               && qint64(pane.m_originalImage.width()) * pane.m_originalImage.height()
                    <= PreviewPane::PdfRenderPixelLimit,
           "PDF render stays within the resolution and pixel limits");

    const PreviewResult blankPdf = PreviewPane::loadPdf(blankPdfPath, QFileInfo(blankPdfPath));
    verify(blankPdf.kind == PreviewResult::Kind::Image
               && blankPdf.image.format() == QImage::Format_RGB32
               && blankPdf.image.pixelColor(0, 0) == QColor(Qt::white)
               && blankPdf.image.pixelColor(blankPdf.image.width() / 2,
                                             blankPdf.image.height() / 2) == QColor(Qt::white),
           "blank PDF page is composited onto opaque white paper");

    const PreviewResult mixedPdf = PreviewPane::loadPdf(mixedPdfPath, QFileInfo(mixedPdfPath));
    verify(mixedPdf.kind == PreviewResult::Kind::Image
               && mixedPdf.image.format() == QImage::Format_RGB32
               && mixedPdf.image.pixelColor(0, 0) == QColor(Qt::white)
               && mixedPdf.image.pixelColor(mixedPdf.image.width() - 1,
                                             mixedPdf.image.height() - 1) == QColor(Qt::white),
           "mixed vector and scan PDF keeps its unpainted page areas opaque white");
    bool hasBlueVector = false;
    bool hasYellowScan = false;
    for (int y = 0; y < mixedPdf.image.height(); ++y) {
        for (int x = 0; x < mixedPdf.image.width(); ++x) {
            const QColor pixel = mixedPdf.image.pixelColor(x, y);
            hasBlueVector |= pixel.blue() > 100 && pixel.blue() > pixel.red() * 2;
            hasYellowScan |= pixel.red() > 150 && pixel.green() > 120 && pixel.blue() < 130;
        }
    }
    verify(hasBlueVector && hasYellowScan,
           "white-paper compositing preserves vector and embedded scan content");

    pane.preview(QUrl::fromLocalFile(brokenPdf.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_message
                   && pane.m_message->text().contains("could not be read");
           }, 3000), "broken PDF has a safe read error");

    pane.preview(QUrl::fromLocalFile(pdfPath), false);
    pane.preview(QUrl::fromLocalFile(text.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_text
                   && pane.m_text->toPlainText() == QStringLiteral("{\"safe\": true}");
           }, 3000), "late PDF result cannot replace a newer selection");

    pane.preview(QUrl::fromLocalFile(pdfPath), false);
    pane.hide();
    QTest::qWait(250);
    verify(pane.m_originalImage.isNull() && pane.m_image->pixmap().isNull(),
           "hiding Preview invalidates and clears an in-flight PDF result");
    pane.show();

    pane.resize(280, 280);
    pane.preview(QUrl::fromLocalFile(pdfPath), false);
    verify(waitForImageSize(QSize(200, 100)), "PDF fits after a narrow resize");
    pane.resize(500, 420);
    QApplication::processEvents();
    verify(pane.m_imageScroll->viewport()->rect().contains(pane.m_image->fittedRect()),
           "PDF stays fitted immediately during resize");
    verify(waitForImageSize(QSize(200, 100)), "PDF remains proportional after resize");

    pane.resize(340, 500);
    QApplication::processEvents();

    pane.preview(QUrl::fromLocalFile(files.filePath("wide.png")), false);
    verify(waitForImageSize(QSize(1600, 400)), "wide image is initially fitted");
    const QSize firstFit = pane.m_image->pixmap().size();
    pane.resize(280, 280);
    QApplication::processEvents(); // Do not wait for the 40 ms async scale timer.
    const QRect immediateFit = pane.m_image->fittedRect();
    const QRect immediateViewport = pane.m_imageScroll->viewport()->rect();
    verify(!immediateFit.isEmpty() && immediateViewport.contains(immediateFit),
           "image fits immediately during drag, before the async scaler finishes");
    verify(qAbs(double(immediateFit.width()) / immediateFit.height() - 4.0) < 0.02,
           "immediate fit preserves aspect ratio");
    verify(waitForImageSize(QSize(1600, 400)) && pane.m_image->pixmap().size() != firstFit,
           "image is refitted from the original after the viewport shrinks");
    pane.resize(500, 500);
    QApplication::processEvents();
    verify(pane.m_imageScroll->viewport()->rect().contains(pane.m_image->fittedRect()),
           "image stays within viewport immediately when expanding");
    verify(QTest::qWaitFor([&] { return pane.m_image->pixmap().width() > firstFit.width(); }, 3000)
               && waitForImageSize(QSize(1600, 400)),
           "image is refitted from the original after the viewport grows");

    for (int width : {270, 410, 290, 500, 280}) {
        pane.resize(width, 450);
        QApplication::processEvents();
        const QRect fitted = pane.m_image->fittedRect();
        verify(!fitted.isEmpty() && pane.m_imageScroll->viewport()->rect().contains(fitted),
               "image remains fitted on every intermediate drag width");
    }

    pane.preview(QUrl::fromLocalFile(binary.fileName()), false);
    verify(QTest::qWaitFor([&] { return pane.m_stack->currentWidget() == pane.m_message
                                      && pane.m_message->text().contains("not yet supported"); }, 3000),
           "unsupported file has a safe fallback");

    pane.preview(QUrl::fromLocalFile(folderPath), true);
    verify(QTest::qWaitFor([&] {
               const QString summary = pane.m_message->text();
               return pane.m_stack->currentWidget() == pane.m_message
                   && summary.contains("Subfolders: 1")
                   && summary.contains("Files: 2")
                   && summary.contains("directly in this folder: 9 B")
                   && summary.contains("Skipped symbolic links: 2");
           }, 3000),
           "folder summary counts only direct regular files without following links");

    pane.preview(QUrl::fromLocalFile(linkedFolderPath), true);
    verify(QTest::qWaitFor([&] {
               return pane.m_message->text().contains("cannot be read");
           }, 3000),
           "selected folder symbolic link is not followed");

    pane.preview(QUrl::fromLocalFile(files.filePath(QStringLiteral("missing"))), true);
    verify(QTest::qWaitFor([&] {
               return pane.m_message->text().contains("cannot be read");
           }, 3000),
           "unreadable or missing folder has a safe error");

    pane.preview(QUrl::fromLocalFile(folderPath), true);
    pane.preview(QUrl::fromLocalFile(text.fileName()), false);
    verify(QTest::qWaitFor([&] {
               return pane.m_stack->currentWidget() == pane.m_text
                   && pane.m_text->toPlainText() == QStringLiteral("{\"safe\": true}");
           }, 3000),
           "late folder result cannot replace a newer file preview");

    const QString limitedPath = files.filePath(QStringLiteral("limited"));
    verify(QDir().mkpath(limitedPath), "create entry-limit fixture");
    bool limitFilesCreated = true;
    for (qsizetype index = 0; index <= PreviewPane::FolderEntryLimit; ++index) {
        QFile entry(QDir(limitedPath).filePath(QString::number(index)));
        if (!entry.open(QIODevice::WriteOnly)) {
            limitFilesCreated = false;
            break;
        }
    }
    verify(limitFilesCreated, "create entry-limit files");
    pane.preview(QUrl::fromLocalFile(limitedPath), true);
    verify(QTest::qWaitFor([&] {
               const QString summary = pane.m_message->text();
               return summary.contains("Files: 10000")
                   && summary.contains("Results are incomplete")
                   && summary.contains("first 10,000 entries");
           }, 10000),
           "folder entry limit produces explicit partial results");

    pane.preview(QUrl(QStringLiteral("sftp://example.invalid/file.txt")), false);
    verify(pane.m_message->text().contains("Remote"), "remote URL is never fetched");

    const QString leftDir = files.filePath(QStringLiteral("left"));
    const QString rightDir = files.filePath(QStringLiteral("right"));
    verify(QDir().mkpath(leftDir) && QDir().mkpath(rightDir), "create pane fixtures");
    QFile leftFile(QDir(leftDir).filePath(QStringLiteral("left.txt")));
    QFile rightFile(QDir(rightDir).filePath(QStringLiteral("right.txt")));
    QFile leftExtra(QDir(leftDir).filePath(QStringLiteral("z-extra.txt")));
    QFile rightExtra(QDir(rightDir).filePath(QStringLiteral("z-extra.txt")));
    verify(leftFile.open(QIODevice::WriteOnly) && rightFile.open(QIODevice::WriteOnly),
           "create pane preview files");
    verify(leftExtra.open(QIODevice::WriteOnly) && rightExtra.open(QIODevice::WriteOnly),
           "create extra pane preview files");
    leftFile.write("left preview");
    rightFile.write("right preview");
    leftExtra.write("left extra");
    rightExtra.write("right extra");
    leftFile.close();
    rightFile.close();
    leftExtra.close();
    rightExtra.close();

    ThisPcWindow window(QUrl::fromLocalFile(leftDir), false);
    window.resize(1200, 700);
    window.show();
    verify(QTest::qWaitFor([&] { return !window.m_primaryPane->listingJob(); }, 5000),
           "primary pane loads");
    window.m_previewAction->setChecked(true);
    verify(window.m_previewPane->isVisible(), "Alt+P action exposes the shared preview pane");
    window.m_directoryList->clearSelection();
    QApplication::processEvents();
    verify(window.m_previewPane->m_title->text().isEmpty(),
           "zero primary selections clear Preview");
    window.m_directoryList->selectionModel()->select(window.m_directoryList->item(0), QItemSelectionModel::Select);
    window.m_directoryList->selectionModel()->select(window.m_directoryList->item(1), QItemSelectionModel::Select);
    QApplication::processEvents();
    verify(window.m_previewPane->m_title->text().isEmpty(),
           "multiple primary selections clear Preview");
    window.m_directoryList->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    verify(QTest::qWaitFor([&] { return window.m_previewPane->m_title->text() == QStringLiteral("left.txt")
                                      && window.m_previewPane->m_text->toPlainText() == QStringLiteral("left preview"); }, 3000),
           "primary selection routes to preview");

    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(rightDir), true);
    verify(QTest::qWaitFor([&] { return !window.m_splitPane->m_job; }, 5000),
           "split pane loads");
    window.setActivePane(ThisPcWindow::PaneId::Split);
    window.m_splitPane->listView()->clearSelection();
    QApplication::processEvents();
    verify(window.m_previewPane->m_title->text().isEmpty(),
           "zero split selections clear Preview");
    window.m_splitPane->listView()->selectionModel()->select(
        window.m_splitPane->listView()->item(0), QItemSelectionModel::Select);
    window.m_splitPane->listView()->selectionModel()->select(
        window.m_splitPane->listView()->item(1), QItemSelectionModel::Select);
    QApplication::processEvents();
    verify(window.m_previewPane->m_title->text().isEmpty(),
           "multiple split selections clear Preview");
    window.m_splitPane->listView()->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    verify(QTest::qWaitFor([&] { return window.m_previewPane->m_title->text() == QStringLiteral("right.txt")
                                      && window.m_previewPane->m_text->toPlainText() == QStringLiteral("right preview"); }, 3000),
           "active split selection routes to the same preview pane");
    verify(window.m_contentSplitter->count() == 2,
           "preview remains outside the two-pane Split View proportions");

    // Every primary breadcrumb is visibly interactive without changing its size.
    const auto primaryCrumbs =
        window.m_breadcrumbFrame->findChildren<QToolButton *>(QStringLiteral("crumbButton"));
    verify(!primaryCrumbs.isEmpty(), "primary breadcrumb exposes individual folder buttons");
    verify(primaryCrumbs.last()->cursor().shape() == Qt::PointingHandCursor,
           "primary folder breadcrumb uses a pointing-hand cursor");

    // The secondary pane exposes full-length, independently clickable labels.
    // It scrolls like the primary pane only when the entire path cannot fit.
    window.resize(2400, 700);
    QApplication::processEvents();
    auto *pathButton = window.m_splitPane->m_breadcrumbButton;
    verify(pathButton->segmentCount() >= 3,
           "secondary breadcrumb exposes independently addressable segments");
    const QRect parentSegment = pathButton->segmentRect(pathButton->segmentCount() - 2);
    verify(!parentSegment.isEmpty()
               && window.m_splitPane->m_breadcrumbScroll->viewport()->rect().contains(
                   pathButton->mapTo(window.m_splitPane->m_breadcrumbScroll->viewport(),
                                     parentSegment.center())),
           "secondary parent folder is fully readable and clickable");
    const QList<int> beforeNavigation = window.m_contentSplitter->sizes();
    QTest::mouseMove(pathButton, parentSegment.center());
    verify(pathButton->cursor().shape() == Qt::PointingHandCursor,
           "secondary breadcrumb highlights an individual folder on hover");
    QTest::mouseClick(pathButton, Qt::LeftButton, Qt::NoModifier, parentSegment.center());
    verify(QTest::qWaitFor([&] {
               return sameLocation(window.m_splitPane->currentUrl(),
                                   QUrl::fromLocalFile(files.path()));
           }, 3000), "clicking a secondary breadcrumb navigates to that folder");
    const QList<int> afterNavigation = window.m_contentSplitter->sizes();
    verify(beforeNavigation.size() == 2 && afterNavigation.size() == 2
               && qAbs(beforeNavigation.at(0) - afterNavigation.at(0)) <= 2,
           "secondary breadcrumb navigation preserves manual Split View widths");

    // Synthetic URLs exercise breadcrumb presentation without enumerating
    // anyone's home directory or creating files outside the temporary fixture.
    const QString homePath = QDir::cleanPath(QDir::homePath());
    const QUrl homeTarget = QUrl::fromLocalFile(
        QDir(homePath).filePath(QStringLiteral("Pobrane/breadcrumb-probe")));
    const auto homeSegments = window.m_splitPane->localPathSegments(homeTarget);
    verify(homeSegments.size() == 3
               && homeSegments.at(0).text == trLocal("Katalog domowy", "Home")
               && sameLocation(homeSegments.at(0).url, QUrl::fromLocalFile(homePath))
               && homeSegments.at(1).text == QStringLiteral("Pobrane")
               && homeSegments.at(2).text == QStringLiteral("breadcrumb-probe")
               && sameLocation(homeSegments.at(2).url, homeTarget),
           "secondary home breadcrumbs match the primary pane without /home/username");
    const auto rootSegments = window.m_splitPane->localPathSegments(
        QUrl::fromLocalFile(QStringLiteral("/tmp/thispc-breadcrumb-probe")));
    verify(!rootSegments.isEmpty() && rootSegments.last().text
               == QStringLiteral("thispc-breadcrumb-probe")
               && sameLocation(rootSegments.last().url,
                               QUrl::fromLocalFile(QStringLiteral("/tmp/thispc-breadcrumb-probe"))),
           "secondary path outside home retains its destination URL");

    SegmentedPathButton compactPath(&window);
    compactPath.setIcon(QIcon());
    compactPath.setSegments({{QStringLiteral("Home"), QUrl::fromLocalFile("/home")},
                             {QStringLiteral("very-long-directory"), QUrl::fromLocalFile("/home/very-long-directory")},
                             {QStringLiteral("end"), QUrl::fromLocalFile("/home/very-long-directory/end")}});
    compactPath.resize(100, 30);
    compactPath.show();
    QApplication::processEvents();
    verify(compactPath.sizeHint().width() > 100
               && !compactPath.segmentRect(0).isEmpty()
               && !compactPath.segmentRect(compactPath.segmentCount() - 1).isEmpty(),
           "narrow breadcrumb keeps complete labels in natural-width geometry");

    PathScrollArea compactScroll(&window);
    compactScroll.setWidgetResizable(false);
    auto *scrollPath = new SegmentedPathButton(&compactScroll);
    scrollPath->setSegments({{QStringLiteral("Home"), QUrl::fromLocalFile("/home")},
                             {QStringLiteral("very-long-directory"), QUrl::fromLocalFile("/home/very-long-directory")},
                             {QStringLiteral("end"), QUrl::fromLocalFile("/home/very-long-directory/end")}});
    compactScroll.setWidget(scrollPath);
    compactScroll.resize(100, 30);
    compactScroll.show();
    QApplication::processEvents();
    auto *scrollBar = compactScroll.horizontalScrollBar();
    verify(scrollBar->maximum() > 0, "long full-name path scrolls inside its allocated width");
    scrollBar->setValue(scrollBar->maximum());
    verify(compactScroll.viewport()->rect().contains(
               scrollPath->mapTo(compactScroll.viewport(),
                                 scrollPath->segmentRect(2).center())),
           "scrolling to end reveals the destination without shortening its name");
    verify(scrollPath->sizeHint().width() == scrollPath->width(),
           "long path keeps natural width without changing its containing pane");

    qInfo("PASS: %d assertions, asynchronous safe preview", checks);
}
