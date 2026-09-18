/*
 * Safe, asynchronous Stage 1 file preview panel.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"

#include <QtConcurrentRun>

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

struct PreviewResult
{
    enum class Kind { Message, Text, Image };
    Kind kind = Kind::Message;
    QString title;
    QString message;
    QString text;
    QImage image;
};

class PreviewPane : public QWidget
{
public:
    static constexpr qint64 TextLimit = 2 * 1024 * 1024;
    static constexpr qint64 ImageLimit = 50 * 1024 * 1024;

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
        m_image = new QLabel(m_imageScroll);
        m_image->setAlignment(Qt::AlignCenter);
        m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_imageScroll->setWidget(m_image);
        m_stack->addWidget(m_imageScroll);
        layout->addWidget(m_stack, 1);

        m_scaleTimer.setSingleShot(true);
        m_scaleTimer.setInterval(40);
        connect(&m_scaleTimer, &QTimer::timeout, this, &PreviewPane::startImageScale);

        showMessage(trLocal("Brak wybranego pliku", "No file selected"));
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
        if (isDirectory) {
            showMessage(trLocal("Podgląd folderów pojawi się w kolejnym etapie.",
                                "Folder previews are planned for a later stage."));
            return;
        }
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
        watcher->setFuture(QtConcurrent::run(&PreviewPane::load, url.toLocalFile()));
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (!m_originalImage.isNull()) scheduleImageScale();
    }

private:
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
            return result;
        }

        result.message = trLocal("Ten format nie jest jeszcze obsługiwany w podglądzie.",
                                 "This format is not yet supported by Preview.");
        return result;
    }

    void showMessage(const QString &message)
    {
        clearImage();
        m_message->setText(message);
        m_stack->setCurrentWidget(m_message);
    }

    void clearImage()
    {
        ++m_scaleRequest;
        m_scaleTimer.stop();
        m_originalImage = QImage();
        m_image->clear();
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
            m_text->setPlainText(result.text);
            m_text->moveCursor(QTextCursor::Start);
            m_stack->setCurrentWidget(m_text);
        } else if (result.kind == PreviewResult::Kind::Image) {
            m_originalImage = result.image;
            m_stack->setCurrentWidget(m_imageScroll);
            scheduleImageScale();
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
    QLabel *m_image = nullptr;
    QImage m_originalImage;
    QTimer m_scaleTimer;
    quint64 m_scaleRequest = 0;
};
