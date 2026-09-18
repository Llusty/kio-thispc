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

    QFile binary(files.filePath("sample.bin"));
    verify(binary.open(QIODevice::WriteOnly), "create unsupported fixture");
    binary.write(QByteArray("a\0b", 3));
    binary.close();

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

    pane.preview(QUrl::fromLocalFile(files.filePath("wide.png")), false);
    verify(waitForImageSize(QSize(1600, 400)), "wide image is initially fitted");
    const QSize firstFit = pane.m_image->pixmap().size();
    pane.resize(280, 280);
    verify(waitForImageSize(QSize(1600, 400)) && pane.m_image->pixmap().size() != firstFit,
           "image is refitted from the original after the viewport shrinks");
    pane.resize(500, 500);
    verify(QTest::qWaitFor([&] { return pane.m_image->pixmap().width() > firstFit.width(); }, 3000)
               && waitForImageSize(QSize(1600, 400)),
           "image is refitted from the original after the viewport grows");

    pane.preview(QUrl::fromLocalFile(binary.fileName()), false);
    verify(QTest::qWaitFor([&] { return pane.m_stack->currentWidget() == pane.m_message
                                      && pane.m_message->text().contains("not yet supported"); }, 3000),
           "unsupported file has a safe fallback");

    pane.preview(QUrl::fromLocalFile(files.path()), true);
    verify(pane.m_stack->currentWidget() == pane.m_message
               && pane.m_message->text().contains("later stage"),
           "folder has a safe fallback");

    pane.preview(QUrl(QStringLiteral("sftp://example.invalid/file.txt")), false);
    verify(pane.m_message->text().contains("Remote"), "remote URL is never fetched");

    const QString leftDir = files.filePath(QStringLiteral("left"));
    const QString rightDir = files.filePath(QStringLiteral("right"));
    verify(QDir().mkpath(leftDir) && QDir().mkpath(rightDir), "create pane fixtures");
    QFile leftFile(QDir(leftDir).filePath(QStringLiteral("left.txt")));
    QFile rightFile(QDir(rightDir).filePath(QStringLiteral("right.txt")));
    verify(leftFile.open(QIODevice::WriteOnly) && rightFile.open(QIODevice::WriteOnly),
           "create pane preview files");
    leftFile.write("left preview");
    rightFile.write("right preview");
    leftFile.close();
    rightFile.close();

    ThisPcWindow window(QUrl::fromLocalFile(leftDir), false);
    window.resize(1200, 700);
    window.show();
    verify(QTest::qWaitFor([&] { return !window.m_directoryJob; }, 5000),
           "primary pane loads");
    window.m_previewAction->setChecked(true);
    verify(window.m_previewPane->isVisible(), "Alt+P action exposes the shared preview pane");
    window.m_directoryList->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    verify(QTest::qWaitFor([&] { return window.m_previewPane->m_title->text() == QStringLiteral("left.txt")
                                      && window.m_previewPane->m_text->toPlainText() == QStringLiteral("left preview"); }, 3000),
           "primary selection routes to preview");

    window.setSplitViewEnabled(true);
    window.m_splitPane->setCurrentUrl(QUrl::fromLocalFile(rightDir), true);
    verify(QTest::qWaitFor([&] { return !window.m_splitPane->m_job; }, 5000),
           "split pane loads");
    window.setActivePane(ThisPcWindow::PaneId::Split);
    window.m_splitPane->listView()->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
    verify(QTest::qWaitFor([&] { return window.m_previewPane->m_title->text() == QStringLiteral("right.txt")
                                      && window.m_previewPane->m_text->toPlainText() == QStringLiteral("right preview"); }, 3000),
           "active split selection routes to the same preview pane");
    verify(window.m_contentSplitter->count() == 2,
           "preview remains outside the two-pane Split View proportions");

    qInfo("PASS: %d assertions, asynchronous safe preview", checks);
}
