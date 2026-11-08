/*
 * Transfer queue UI widgets and settings components.
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "transferbandwidthlimiter.h"
#include "transferqueuecontroller.h"
#include "transferqueuesettings.h"
#include "transfertaskdescriptor.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QPointer>
#include <QResizeEvent>
#include <QSizePolicy>

// 0.44 Stage 3-7: the scheduler connection to production file operations.
inline bool TransferQueueProductionDispatchConnected = false;

inline bool isTransferQueueProductionDispatchConnected()
{
    const QString env = qEnvironmentVariable("THISPC_TRANSFER_QUEUE_PRODUCTION").trimmed();
    if (env == QLatin1String("0") || env.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    if (TransferQueueProductionDispatchConnected) {
        return true;
    }
    if (env == QLatin1String("1") || env.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return false;
}

inline void setTransferQueueProductionDispatchConnected(bool connected)
{
    TransferQueueProductionDispatchConnected = connected;
}

inline void activateTransferQueueProductionDispatchByDefault()
{
    const QString env = qEnvironmentVariable("THISPC_TRANSFER_QUEUE_PRODUCTION").trimmed();
    if (env == QLatin1String("0") || env.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
        TransferQueueProductionDispatchConnected = false;
        return;
    }
    TransferQueueProductionDispatchConnected = true;
}

class DemoTransferJob final : public KJob
{
    Q_OBJECT

public:
    explicit DemoTransferJob(quint64 totalBytes, int durationMs, QObject *parent = nullptr,
                             bool suspendable = false)
        : KJob(parent)
        , m_totalBytes(totalBytes)
        , m_durationMs(qMax(50, durationMs))
    {
        setCapabilities(suspendable ? (KJob::Killable | KJob::Suspendable) : KJob::Killable);
    }

    Q_INVOKABLE void setBandwidthLimiter(std::shared_ptr<TransferBandwidthLimiter> limiter, quint64 taskId = 0)
    {
        m_limiter = limiter;
        m_taskId = taskId;
    }

    void start() override
    {
        setTotalAmount(KJob::Bytes, m_totalBytes);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this] {
            triggerStep();
        });
        m_timer->start(qMax(10, m_durationMs / m_maxSteps));
        triggerStep();
    }

protected:
    void triggerStep()
    {
        if (m_waitingForTokens) return;

        const quint64 chunkBytes = m_totalBytes / m_maxSteps;
        if (m_limiter && m_limiter->isEnabled()) {
            m_waitingForTokens = true;
            m_limiter->requestTokens(m_taskId, static_cast<qint64>(chunkBytes), [this, chunkBytes](qint64 granted) {
                m_waitingForTokens = false;
                if (granted > 0) {
                    advanceProgress(chunkBytes);
                }
            });
        } else {
            advanceProgress(chunkBytes);
        }
    }

    void advanceProgress(quint64 /*chunkBytes*/)
    {
        m_step++;
        const quint64 processed = (m_totalBytes * m_step) / m_maxSteps;
        setProcessedAmount(KJob::Bytes, processed);
        emitPercent(processed, m_totalBytes);
        const int intervalMs = m_timer ? m_timer->interval() : 0;
        if (intervalMs > 0) {
            emitSpeed(static_cast<unsigned long>(
                (m_totalBytes / m_maxSteps) * 1000ULL / static_cast<quint64>(intervalMs)));
        }
        if (m_step >= m_maxSteps) {
            if (m_timer) m_timer->stop();
            emitResult();
        }
    }

    bool doKill() override
    {
        if (m_timer) {
            m_timer->stop();
        }
        if (m_limiter && m_taskId != 0) {
            m_limiter->cancelTask(m_taskId);
        }
        setError(KJob::KilledJobError);
        setErrorText(trLocal("Zadanie demonstracyjne anulowane", "Demonstration task cancelled"));
        emitResult();
        return true;
    }

    // Only reachable when constructed as suspendable: the in-memory timer is
    // really stopped, so the reported pause is genuine for this demo job.
    bool doSuspend() override
    {
        if (m_timer) {
            m_timer->stop();
        }
        return true;
    }

    bool doResume() override
    {
        if (m_timer && m_step < m_maxSteps) {
            m_timer->start();
        }
        return true;
    }

private:
    quint64 m_totalBytes = 100 * 1024 * 1024;
    int m_durationMs = 15000;
    int m_step = 0;
    const int m_maxSteps = 100;
    QTimer *m_timer = nullptr;
    std::shared_ptr<TransferBandwidthLimiter> m_limiter;
    quint64 m_taskId = 0;
    bool m_waitingForTokens = false;
};

// The demo is available only in a build configured with
// -DTHISPC_ENABLE_TRANSFER_QUEUE_DEMO=ON *and* when the process is started
// with THISPC_TRANSFER_QUEUE_DEMO=1. Normal builds always return false.
inline bool isTransferQueueDemoMode()
{
#if defined(THISPC_ENABLE_TRANSFER_QUEUE_DEMO) || defined(THISPC_TEST_HARNESS)
    return qEnvironmentVariable("THISPC_TRANSFER_QUEUE_DEMO") == QLatin1String("1");
#else
    return false;
#endif
}

// Isolated settings store for the demo (never the user's real profile).
inline QString transferQueueDemoSettingsPath()
{
    const QString overridePath = qEnvironmentVariable("THISPC_TRANSFER_QUEUE_DEMO_SETTINGS");
    if (!overridePath.isEmpty()) {
        return overridePath;
    }
    return QDir::tempPath() + QStringLiteral("/thispc-transfer-queue-demo/settings.ini");
}

inline std::shared_ptr<QSettings> transferQueueDemoSettingsStore()
{
    if (!isTransferQueueDemoMode()) {
        return {};
    }
    static std::shared_ptr<QSettings> store;
    if (!store) {
        const QString path = transferQueueDemoSettingsPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        store = std::make_shared<QSettings>(path, QSettings::IniFormat);
    }
    return store;
}

inline QString transferQueueDrainingText(int active, int limit)
{
    if (isPolish()) {
        const int mod10 = active % 10;
        const int mod100 = active % 100;
        const bool few = active == 1
            || (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14));
        return QStringLiteral("%1 %2 — nowy limit: %3. Kolejne zadania oczekują.")
            .arg(active)
            .arg(few ? QStringLiteral("aktywne") : QStringLiteral("aktywnych"))
            .arg(limit);
    }
    return QStringLiteral("%1 active — new limit: %2. Later tasks are queued.")
        .arg(active)
        .arg(limit);
}

inline QString transferQueueDemoNoticeText()
{
    return trLocal(
        "Kolejka demonstracyjna — ustawienia nie wpływają na bieżące operacje plikowe.",
        "Demo queue — these settings do not affect current file operations.");
}

// Breaks long continuous tokens (paths, hashes, uninterrupted text) at reasonable
// character boundaries (e.g. slashes or maxLineLen characters) so tooltips never
// blow out across the whole screen in KDE Plasma/Wayland and HiDPI displays.
inline QString wrapTooltipChunk(const QString &rawText, int maxLineLen = 65)
{
    if (rawText.isEmpty()) {
        return {};
    }
    const QStringList rawLines = rawText.split(QLatin1Char('\n'));
    QStringList formattedLines;

    for (const QString &line : rawLines) {
        if (line.isEmpty()) {
            formattedLines.append(QString());
            continue;
        }
        const QStringList words = line.split(QLatin1Char(' '));
        QString currentLine;

        for (QString word : words) {
            while (word.length() > maxLineLen) {
                int splitIdx = maxLineLen;
                const int slashIdx = word.left(maxLineLen).lastIndexOf(QLatin1Char('/'));
                if (slashIdx > 25) {
                    splitIdx = slashIdx + 1;
                } else {
                    const int backslashIdx = word.left(maxLineLen).lastIndexOf(QLatin1Char('\\'));
                    if (backslashIdx > 25) {
                        splitIdx = backslashIdx + 1;
                    }
                }
                if (!currentLine.isEmpty()) {
                    currentLine += QLatin1Char(' ');
                }
                currentLine += word.left(splitIdx);
                formattedLines.append(currentLine.toHtmlEscaped());
                currentLine.clear();
                word = word.mid(splitIdx);
            }
            if (word.isEmpty()) {
                continue;
            }
            if (!currentLine.isEmpty() && currentLine.length() + 1 + word.length() > maxLineLen) {
                formattedLines.append(currentLine.toHtmlEscaped());
                currentLine = word;
            } else {
                if (!currentLine.isEmpty()) {
                    currentLine += QLatin1Char(' ');
                }
                currentLine += word;
            }
        }
        if (!currentLine.isEmpty()) {
            formattedLines.append(currentLine.toHtmlEscaped());
        }
    }

    return formattedLines.join(QStringLiteral("<br/>"));
}

inline QString formatPathTooltip(const QString &sourceLabel, const QString &sourcePath,
                                 const QString &destLabel, const QString &destPath)
{
    QString html = QStringLiteral("<table width=\"520\" cellpadding=\"0\" cellspacing=\"0\"><tr><td>");
    if (!sourcePath.isEmpty()) {
        html += QStringLiteral("<b>%1:</b><br/>%2")
                    .arg(sourceLabel.toHtmlEscaped(), wrapTooltipChunk(sourcePath));
    }
    if (!destPath.isEmpty()) {
        if (!sourcePath.isEmpty()) {
            html += QStringLiteral("<br/><br/>");
        }
        html += QStringLiteral("<b>%1:</b><br/>%2")
                    .arg(destLabel.toHtmlEscaped(), wrapTooltipChunk(destPath));
    }
    html += QStringLiteral("</td></tr></table>");
    return html;
}

inline QString formatSingleValueTooltip(const QString &label, const QString &value)
{
    if (value.length() <= 50 && !value.contains(QLatin1Char('\n'))) {
        return value;
    }
    QString html = QStringLiteral("<table width=\"520\" cellpadding=\"0\" cellspacing=\"0\"><tr><td>");
    if (!label.isEmpty()) {
        html += QStringLiteral("<b>%1:</b><br/>").arg(label.toHtmlEscaped());
    }
    html += wrapTooltipChunk(value);
    html += QStringLiteral("</td></tr></table>");
    return html;
}

// QLabel that never widens its parent: it elides its full text to the
// available width and exposes the full text through the tooltip.
class TransferQueueElidedLabel : public QLabel
{
    Q_OBJECT

public:
    explicit TransferQueueElidedLabel(QWidget *parent = nullptr,
                                      Qt::TextElideMode mode = Qt::ElideRight)
        : QLabel(parent)
        , m_mode(mode)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
    }

    explicit TransferQueueElidedLabel(const QString &text,
                                      QWidget *parent = nullptr,
                                      Qt::TextElideMode mode = Qt::ElideRight)
        : QLabel(parent)
        , m_mode(mode)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setFullText(text);
    }

    void setFullText(const QString &text, bool autoTooltip = true)
    {
        m_fullText = text;
        if (autoTooltip) {
            setToolTip(formatSingleValueTooltip(QString(), text));
        }
        updateElidedText();
    }

    void setText(const QString &text)
    {
        setFullText(text);
    }

    QString fullText() const { return m_fullText; }

    void setMode(Qt::TextElideMode mode)
    {
        if (m_mode != mode) {
            m_mode = mode;
            updateElidedText();
        }
    }

    Qt::TextElideMode mode() const { return m_mode; }

    QSize minimumSizeHint() const override
    {
        return QSize(24, fontMetrics().height());
    }

    QSize sizeHint() const override
    {
        return QSize(qMin(fontMetrics().horizontalAdvance(m_fullText) + 4, 320),
                     fontMetrics().height());
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateElidedText();
    }

    void showEvent(QShowEvent *event) override
    {
        QLabel::showEvent(event);
        updateElidedText();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange) {
            updateElidedText();
        }
    }

private:
    void updateElidedText()
    {
        const int available = qMax(0, contentsRect().width());
        QLabel::setText(available > 0
                            ? fontMetrics().elidedText(m_fullText, m_mode, available)
                            : m_fullText);
    }

    QString m_fullText;
    Qt::TextElideMode m_mode = Qt::ElideRight;
};

inline void enqueueDemoTasks(TransferQueueController *controller)
{
    if (!controller) {
        return;
    }

    // Task 1: 4K Video file (Copy, 2.4 GB, Normal priority, supports Pause/Resume)
    TransferTaskDescriptor t1;
    t1.title = QStringLiteral("4K_Render_Video.mp4");
    t1.type = TransferOperationType::Copy;
    t1.backend = TransferBackendKind::LocalNative;
    t1.sources = {QUrl::fromLocalFile(QStringLiteral("/home/user/Wideo/4K_Render_Video.mp4"))};
    t1.destination = QUrl::fromLocalFile(QStringLiteral("/run/media/user/Backup/Wideo/"));
    t1.priority = TransferPriority::Normal;
    t1.startMode = TransferJobStartMode::ControllerStarts;
    t1.capabilities.canCancelRunning = true;
    t1.capabilities.canPause = true;
    t1.capabilities.canResume = true;
    t1.jobFactory = []() -> KJob * {
        return new DemoTransferJob(2576980377ULL, 60000, nullptr, /*suspendable=*/true);
    };
    controller->enqueue(t1);

    // Task 2: Database report (Move, 850 MB, High priority, cannot pause)
    TransferTaskDescriptor t2;
    t2.title = QStringLiteral("Baza_Danych_Raport.sqlite");
    t2.type = TransferOperationType::Move;
    t2.backend = TransferBackendKind::LocalNative;
    t2.sources = {QUrl::fromLocalFile(QStringLiteral("/home/user/Dokumenty/Baza_Danych_Raport.sqlite"))};
    t2.destination = QUrl::fromLocalFile(QStringLiteral("/run/media/user/Backup/Dokumenty/"));
    t2.priority = TransferPriority::High;
    t2.startMode = TransferJobStartMode::ControllerStarts;
    t2.capabilities.canCancelRunning = true;
    t2.capabilities.canPause = false;
    t2.capabilities.canResume = false;
    t2.jobFactory = []() -> KJob * {
        return new DemoTransferJob(891289600ULL, 45000, nullptr, /*suspendable=*/false);
    };
    controller->enqueue(t2);

    // Task 3: Very long filename to test label wrapping, eliding, and tooltips (supports Pause)
    TransferTaskDescriptor t3;
    t3.title = QStringLiteral("Bardzo_dluga_nazwa_pliku_ze_sciezkami_do_weryfikacji_elidingu_tekstu_oraz_zawijania_etykiet_w_interfejsie_uzytkownika_thispc_044.dat");
    t3.type = TransferOperationType::Copy;
    t3.backend = TransferBackendKind::LocalNative;
    t3.sources = {QUrl::fromLocalFile(QStringLiteral("/home/user/Pobrane/Kolekcja_Danych_2026/Projekt_Specjalny_Dluga_Sciezka/Bardzo_dluga_nazwa_pliku_ze_sciezkami_do_weryfikacji_elidingu_tekstu_oraz_zawijania_etykiet_w_interfejsie_uzytkownika_thispc_044.dat"))};
    t3.destination = QUrl::fromLocalFile(QStringLiteral("/run/media/user/Archiwum_Zewnetrzne/Kopia_Zapasowa_Struktura_Katalogow_Gleboka/"));
    t3.priority = TransferPriority::Normal;
    t3.startMode = TransferJobStartMode::ControllerStarts;
    t3.capabilities.canCancelRunning = true;
    t3.capabilities.canPause = true;
    t3.capabilities.canResume = true;
    t3.jobFactory = []() -> KJob * {
        return new DemoTransferJob(1288490188ULL, 55000, nullptr, /*suspendable=*/true);
    };
    controller->enqueue(t3);

    // Task 4: Archive (Copy, 500 MB, Low priority, cannot pause)
    TransferTaskDescriptor t4;
    t4.title = QStringLiteral("Projekt_Dokumenty_Archiwum.tar.zst");
    t4.type = TransferOperationType::Copy;
    t4.backend = TransferBackendKind::LocalNative;
    t4.sources = {QUrl::fromLocalFile(QStringLiteral("/home/user/Archiwa/Projekt_Dokumenty_Archiwum.tar.zst"))};
    t4.destination = QUrl::fromLocalFile(QStringLiteral("/run/media/user/WspolnyFolder/Projekt_Dokumenty_Archiwum.tar.zst"));
    t4.priority = TransferPriority::Low;
    t4.startMode = TransferJobStartMode::ControllerStarts;
    t4.capabilities.canCancelRunning = true;
    t4.capabilities.canPause = false;
    t4.capabilities.canResume = false;
    t4.jobFactory = []() -> KJob * {
        return new DemoTransferJob(524288000ULL, 40000, nullptr, /*suspendable=*/false);
    };
    controller->enqueue(t4);

    // Task 5: Reservation conflict test targeting the same file path as Task 4
    TransferTaskDescriptor t5;
    t5.title = QStringLiteral("Konflikt_Celu_Wspolna_Sciezka.dat");
    t5.type = TransferOperationType::Move;
    t5.backend = TransferBackendKind::LocalNative;
    t5.sources = {QUrl::fromLocalFile(QStringLiteral("/home/user/Tymczasowe/Konflikt_Celu_Wspolna_Sciezka.dat"))};
    t5.destination = QUrl::fromLocalFile(QStringLiteral("/run/media/user/WspolnyFolder/Projekt_Dokumenty_Archiwum.tar.zst"));
    t5.priority = TransferPriority::Normal;
    t5.startMode = TransferJobStartMode::ControllerStarts;
    t5.capabilities.canCancelRunning = true;
    t5.capabilities.canPause = true;
    t5.capabilities.canResume = true;
    t5.jobFactory = []() -> KJob * {
        return new DemoTransferJob(314572800ULL, 35000, nullptr, /*suspendable=*/true);
    };
    controller->enqueue(t5);
}

#ifdef THISPC_TEST_HARNESS
inline std::optional<bool> &clearHistoryPromptOverrideForTest()
{
    static std::optional<bool> s_override;
    return s_override;
}
#endif

inline bool promptClearHistoryConfirmation(QWidget *parent = nullptr)
{
#ifdef THISPC_TEST_HARNESS
    if (clearHistoryPromptOverrideForTest().has_value()) {
        return *clearHistoryPromptOverrideForTest();
    }
    if (qEnvironmentVariableIsSet("THISPC_TEST_HARNESS") || QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        return true;
    }
#endif
    QMessageBox msgBox(parent);
    msgBox.setObjectName(QStringLiteral("clearHistoryConfirmationBox"));
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setWindowTitle(isPolish() ? QStringLiteral("Wyczyść historię operacji")
                                     : QStringLiteral("Clear Operation History"));
    msgBox.setText(isPolish()
        ? QStringLiteral("Czy na pewno chcesz wyczyścić zapisaną historię operacji?")
        : QStringLiteral("Are you sure you want to clear the saved operation history?"));
    msgBox.setInformativeText(isPolish()
        ? QStringLiteral("Usunięte zostaną wyłącznie wpisy z dziennika historii transferów.\nŻadne pliki na dysku (ani źródłowe, ani docelowe) nie zostaną usunięte ani zmodyfikowane.")
        : QStringLiteral("Only history log entries will be removed.\nNo actual files on disk (neither source nor destination) will be deleted or modified."));

    auto *confirmBtn = msgBox.addButton(
        isPolish() ? QStringLiteral("Wyczyść historię") : QStringLiteral("Clear History"),
        QMessageBox::AcceptRole);
    confirmBtn->setObjectName(QStringLiteral("confirmClearHistoryButton"));

    auto *cancelBtn = msgBox.addButton(
        isPolish() ? QStringLiteral("Anuluj") : QStringLiteral("Cancel"),
        QMessageBox::RejectRole);
    cancelBtn->setObjectName(QStringLiteral("cancelClearHistoryButton"));

    msgBox.setDefaultButton(cancelBtn);

    msgBox.exec();
    return msgBox.clickedButton() == confirmBtn;
}

class TransferQueueSettingsWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TransferQueueSettingsWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("transferQueueSettingsWidget"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(2);

        m_groupBox = new QGroupBox(trLocal("Tryb wykonywania", "Execution mode"), this);
        m_groupBox->setObjectName(QStringLiteral("transferQueueSettingsGroup"));
        m_groupBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        auto *groupLayout = new QVBoxLayout(m_groupBox);
        groupLayout->setContentsMargins(8, 2, 8, 3);
        groupLayout->setSpacing(2);

        auto *controlsRow = new QHBoxLayout;
        controlsRow->setContentsMargins(0, 0, 0, 0);
        controlsRow->setSpacing(8);

        m_sequentialRadio = new QRadioButton(
            trLocal("Szeregowy", "Sequential"), m_groupBox);
        m_sequentialRadio->setObjectName(QStringLiteral("transferQueueSequentialRadio"));
        m_sequentialRadio->setToolTip(
            trLocal("Wykonywanie jednego transferu na raz (1 aktywny)",
                    "Run one transfer at a time (1 active)"));

        m_parallelRadio = new QRadioButton(
            trLocal("Równoległy", "Parallel"), m_groupBox);
        m_parallelRadio->setObjectName(QStringLiteral("transferQueueParallelRadio"));
        m_parallelRadio->setToolTip(
            trLocal("Równoczesne wykonywanie transferów zgodnie z limitem",
                    "Run concurrent transfers up to the configured limit"));

        m_modeGroup = new QButtonGroup(this);
        m_modeGroup->addButton(m_sequentialRadio, static_cast<int>(TransferQueueMode::Sequential));
        m_modeGroup->addButton(m_parallelRadio, static_cast<int>(TransferQueueMode::Parallel));

        controlsRow->addWidget(m_sequentialRadio);
        controlsRow->addWidget(m_parallelRadio);

        controlsRow->addSpacing(8);

        m_limitLabel = new QLabel(
            trLocal("Limit jednoczesnych:", "Concurrent limit:"), m_groupBox);
        m_limitLabel->setObjectName(QStringLiteral("transferQueueLimitLabel"));
        controlsRow->addWidget(m_limitLabel);

        m_limitSpin = new QSpinBox(m_groupBox);
        m_limitSpin->setObjectName(QStringLiteral("transferQueueLimitSpin"));
        m_limitSpin->setRange(TransferQueueMinParallelLimit, TransferQueueMaxParallelLimit);
        m_limitSpin->setValue(TransferQueueDefaultParallelLimit);
        m_limitSpin->setToolTip(
            trLocal("Maksymalna liczba transferów wykonywanych jednocześnie (1–8)",
                    "Maximum number of concurrent transfers (1–8)"));
        m_limitSpin->setFixedWidth(64);
        m_limitSpin->setMinimumHeight(20);
        controlsRow->addWidget(m_limitSpin);

        controlsRow->addStretch(1);
        groupLayout->addLayout(controlsRow);

        // Stage 6: Bandwidth Throttling controls row
        auto *bandwidthRow = new QHBoxLayout;
        bandwidthRow->setContentsMargins(0, 0, 0, 0);
        bandwidthRow->setSpacing(6);

        m_bandwidthCheck = new QCheckBox(
            trLocal("Ogranicz prędkość:", "Limit speed:"), m_groupBox);
        m_bandwidthCheck->setObjectName(QStringLiteral("transferQueueBandwidthCheck"));
        m_bandwidthCheck->setToolTip(
            trLocal("Ogranicz prędkość transferu — ogranicza łączną przepustowość wszystkich zadań",
                    "Limit transfer speed — limits global bandwidth across all queued tasks"));
        bandwidthRow->addWidget(m_bandwidthCheck);

        m_bandwidthSpin = new QSpinBox(m_groupBox);
        m_bandwidthSpin->setObjectName(QStringLiteral("transferQueueBandwidthSpin"));
        m_bandwidthSpin->setRange(1, 9999);
        m_bandwidthSpin->setValue(10);
        m_bandwidthSpin->setFixedWidth(64);
        m_bandwidthSpin->setMinimumHeight(20);
        m_bandwidthSpin->setEnabled(false);
        m_bandwidthSpin->setToolTip(
            trLocal("Wartość limitu prędkości", "Speed limit value"));
        bandwidthRow->addWidget(m_bandwidthSpin);

        m_bandwidthUnitCombo = new QComboBox(m_groupBox);
        m_bandwidthUnitCombo->setObjectName(QStringLiteral("transferQueueBandwidthUnitCombo"));
        m_bandwidthUnitCombo->addItem(QStringLiteral("MiB/s"), static_cast<int>(BandwidthUnit::MiB_s));
        m_bandwidthUnitCombo->addItem(QStringLiteral("KiB/s"), static_cast<int>(BandwidthUnit::KiB_s));
        m_bandwidthUnitCombo->setFixedWidth(74);
        m_bandwidthUnitCombo->setMinimumHeight(20);
        m_bandwidthUnitCombo->setEnabled(false);
        m_bandwidthUnitCombo->setToolTip(
            trLocal("Jednostka prędkości transferu", "Transfer speed unit"));
        bandwidthRow->addWidget(m_bandwidthUnitCombo);

        m_bandwidthInfoLabel = new QLabel(m_groupBox);
        m_bandwidthInfoLabel->setObjectName(QStringLiteral("transferQueueBandwidthInfoLabel"));
        m_bandwidthInfoLabel->setForegroundRole(QPalette::PlaceholderText);
        QFont infoFont = m_bandwidthInfoLabel->font();
        infoFont.setPointSize(qMax(8, infoFont.pointSize() - 1));
        m_bandwidthInfoLabel->setFont(infoFont);
        m_bandwidthInfoLabel->setText(trLocal("(zadania lokalne)", "(local tasks)"));
        m_bandwidthInfoLabel->setToolTip(
            trLocal("Ograniczenie prędkości dotyczy lokalnych operacji we/wy",
                    "Bandwidth throttling applies to local I/O operations"));
        bandwidthRow->addWidget(m_bandwidthInfoLabel);

        bandwidthRow->addStretch(1);
        groupLayout->addLayout(bandwidthRow);

        m_closeOnCompleteCheckBox = new QCheckBox(
            trLocal("Zamknij okno po zakończeniu wszystkich operacji",
                    "Close window when all operations complete"), m_groupBox);
        m_closeOnCompleteCheckBox->setObjectName(QStringLiteral("closeWindowOnCompleteCheckBox"));
        m_closeOnCompleteCheckBox->setToolTip(
            trLocal("Automatycznie zamyka okno operacji po zakończeniu wszystkich zadań, jeśli nie wystąpiły błędy",
                    "Automatically closes the operations window when all tasks finish without errors"));
        groupLayout->addWidget(m_closeOnCompleteCheckBox);

        m_desktopNotificationsCheckBox = new QCheckBox(
            trLocal("Powiadomienia na pulpicie o zakończeniu i błędach",
                    "Desktop notifications for completion and errors"), m_groupBox);
        m_desktopNotificationsCheckBox->setObjectName(QStringLiteral("desktopNotificationsCheckBox"));
        m_desktopNotificationsCheckBox->setToolTip(
            trLocal("Wyświetla powiadomienia KDE Plasma po zakończeniu transferów w tle oraz przy błędach",
                    "Displays KDE Plasma notifications when background transfers complete and on errors"));
        groupLayout->addWidget(m_desktopNotificationsCheckBox);

        // Stage 2.5: History settings row
        auto *historyRow = new QHBoxLayout;
        historyRow->setContentsMargins(0, 0, 0, 0);
        historyRow->setSpacing(8);

        m_historyEnabledCheckBox = new QCheckBox(
            trLocal("Zapisuj historię operacji", "Save operation history"), m_groupBox);
        m_historyEnabledCheckBox->setObjectName(QStringLiteral("historyEnabledCheckBox"));
        m_historyEnabledCheckBox->setChecked(true);
        m_historyEnabledCheckBox->setToolTip(
            trLocal("Trwały zapis zakończonych operacji plikowych w historii",
                    "Persistent recording of completed file operations in history"));
        historyRow->addWidget(m_historyEnabledCheckBox);

        m_historyRetentionLabel = new QLabel(
            trLocal("Maksymalna liczba:", "Max operations:"), m_groupBox);
        m_historyRetentionLabel->setObjectName(QStringLiteral("historyRetentionLabel"));
        historyRow->addWidget(m_historyRetentionLabel);

        m_historyRetentionCombo = new QComboBox(m_groupBox);
        m_historyRetentionCombo->setObjectName(QStringLiteral("historyRetentionCombo"));
        m_historyRetentionCombo->addItem(QStringLiteral("50"), 50);
        m_historyRetentionCombo->addItem(QStringLiteral("100"), 100);
        m_historyRetentionCombo->addItem(QStringLiteral("200"), 200);
        m_historyRetentionCombo->addItem(QStringLiteral("500"), 500);
        m_historyRetentionCombo->setCurrentIndex(2); // 200 default
        m_historyRetentionCombo->setFixedWidth(64);
        m_historyRetentionCombo->setMinimumHeight(20);
        m_historyRetentionCombo->setToolTip(
            trLocal("Limit operacji w historii (50, 100, 200, 500)",
                    "Limit of operations in history (50, 100, 200, 500)"));
        historyRow->addWidget(m_historyRetentionCombo);

        m_clearHistoryButton = new QPushButton(
            trLocal("Wyczyść historię…", "Clear history…"), m_groupBox);
        m_clearHistoryButton->setObjectName(QStringLiteral("settingsClearHistoryButton"));
        m_clearHistoryButton->setMinimumHeight(20);
        m_clearHistoryButton->setToolTip(
            trLocal("Wyczyść zapisaną historię operacji (wymaga potwierdzenia)",
                    "Clear saved operation history (requires confirmation)"));
        historyRow->addWidget(m_clearHistoryButton);

        historyRow->addStretch(1);
        groupLayout->addLayout(historyRow);

        m_drainingLabel = new QLabel(m_groupBox);
        m_drainingLabel->setObjectName(QStringLiteral("transferQueueDrainingLabel"));
        m_drainingLabel->setWordWrap(true);
        // Palette text on a translucent amber tint: readable in light and
        // dark KDE colour schemes. This is information, not an error.
        m_drainingLabel->setStyleSheet(QStringLiteral(
            "QLabel { background-color: rgba(230, 160, 40, 0.18); "
            "border: 1px solid rgba(230, 160, 40, 0.55); border-radius: 4px; "
            "padding: 4px 8px; }"));
        m_drainingLabel->hide();
        groupLayout->addWidget(m_drainingLabel);

        mainLayout->addWidget(m_groupBox);

        connect(m_sequentialRadio, &QRadioButton::toggled, this, [this](bool checked) {
            m_limitSpin->setEnabled(!checked);
            m_limitLabel->setEnabled(!checked);
            if (!m_updating && m_controller) {
                m_controller->setExecutionMode(checked ? TransferQueueMode::Sequential : TransferQueueMode::Parallel);
                persistSettings();
                updateDrainingState();
            }
        });

        connect(m_limitSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int val) {
            if (!m_updating && m_controller) {
                m_controller->setParallelConcurrencyLimit(val);
                persistSettings();
                updateDrainingState();
            }
        });

        auto updateLimiterFromUi = [this] {
            if (m_updating || !m_controller) return;
            const bool enabled = m_bandwidthCheck->isChecked();
            const int val = m_bandwidthSpin->value();
            const auto unit = static_cast<BandwidthUnit>(m_bandwidthUnitCombo->currentData().toInt());
            const quint64 bytesPerSec = bandwidthToBytesPerSec(val, unit);
            m_controller->setBandwidthLimit(enabled, bytesPerSec, unit, val);
            persistSettings();
        };

        connect(m_bandwidthCheck, &QCheckBox::toggled, this, [this, updateLimiterFromUi](bool checked) {
            m_bandwidthSpin->setEnabled(checked);
            m_bandwidthUnitCombo->setEnabled(checked);
            updateLimiterFromUi();
        });

        connect(m_bandwidthSpin, qOverload<int>(&QSpinBox::valueChanged), this, [updateLimiterFromUi](int) {
            updateLimiterFromUi();
        });

        connect(m_bandwidthUnitCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [updateLimiterFromUi](int) {
            updateLimiterFromUi();
        });

        connect(m_closeOnCompleteCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (!m_updating && m_controller) {
                m_controller->setCloseWindowOnComplete(checked);
                persistSettings();
            }
            Q_EMIT closeWindowOnCompleteToggled(checked);
        });

        connect(m_desktopNotificationsCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            if (!m_updating && m_controller) {
                m_controller->setDesktopNotificationsEnabled(checked);
                persistSettings();
            }
        });

        connect(m_historyEnabledCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
            m_historyRetentionLabel->setEnabled(checked);
            m_historyRetentionCombo->setEnabled(checked);
            if (!m_updating && m_controller) {
                m_controller->setHistoryEnabled(checked);
                persistSettings();
            }
        });

        connect(m_historyRetentionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (!m_updating && m_controller) {
                const int val = m_historyRetentionCombo->currentData().toInt();
                m_controller->setHistoryMaxRecords(val);
                persistSettings();
            }
        });

        connect(m_clearHistoryButton, &QPushButton::clicked, this, [this] {
            if (m_controller) {
                if (promptClearHistoryConfirmation(this)) {
                    m_controller->clearHistory(true);
                }
            }
        });

        m_parallelRadio->setChecked(true);
    }

    void setController(TransferQueueController *controller)
    {
        if (m_controller == controller) {
            return;
        }

        if (m_controller) {
            disconnect(m_controller, nullptr, this, nullptr);
        }
        m_controller = controller;
        if (m_controller) {
            connect(m_controller, &TransferQueueController::modeChanged, this, [this](TransferQueueMode) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::parallelLimitChanged, this, [this](int) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::bandwidthLimitChanged, this, [this](bool, quint64) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::closeWindowOnCompleteChanged, this, [this](bool) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::desktopNotificationsEnabledChanged, this, [this](bool) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::historySettingsChanged, this, [this](bool, int) {
                refreshFromController();
            });
            connect(m_controller, &TransferQueueController::drainingChanged, this, [this](bool) {
                updateDrainingState();
            });
            connect(m_controller, &TransferQueueController::queueChanged, this, [this] {
                updateDrainingState();
            });
            refreshFromController();
        }
    }

    TransferQueueController *controller() const { return m_controller; }

    void refreshFromController()
    {
        if (!m_controller) {
            return;
        }

        m_updating = true;
        const TransferQueueMode mode = m_controller->executionMode();
        if (mode == TransferQueueMode::Sequential) {
            m_sequentialRadio->setChecked(true);
            m_limitSpin->setEnabled(false);
            m_limitLabel->setEnabled(false);
        } else {
            m_parallelRadio->setChecked(true);
            m_limitSpin->setEnabled(true);
            m_limitLabel->setEnabled(true);
        }
        m_limitSpin->setValue(m_controller->parallelConcurrencyLimit());

        const bool bwEnabled = m_controller->isBandwidthLimitEnabled();
        m_bandwidthCheck->setChecked(bwEnabled);
        m_bandwidthSpin->setEnabled(bwEnabled);
        m_bandwidthUnitCombo->setEnabled(bwEnabled);

        const quint64 bwBytes = m_controller->bandwidthLimitBytesPerSec();
        if (bwBytes > 0) {
            BandwidthUnit unit = m_controller->bandwidthLimitUnit();
            int val = m_controller->bandwidthLimitValue();
            if (val <= 0 || bandwidthToBytesPerSec(val, unit) != bwBytes) {
                val = bytesPerSecToUnitValue(bwBytes, unit);
            }
            m_bandwidthSpin->setValue(val);
            const int uIdx = m_bandwidthUnitCombo->findData(static_cast<int>(unit));
            if (uIdx >= 0) m_bandwidthUnitCombo->setCurrentIndex(uIdx);
        }

        m_closeOnCompleteCheckBox->setChecked(m_controller->closeWindowOnComplete());
        m_desktopNotificationsCheckBox->setChecked(m_controller->desktopNotificationsEnabled());

        m_historyEnabledCheckBox->setChecked(m_controller->isHistoryEnabled());
        const int maxRec = m_controller->historyMaxRecords();
        const int idx = m_historyRetentionCombo->findData(maxRec);
        if (idx >= 0) m_historyRetentionCombo->setCurrentIndex(idx);
        m_historyRetentionLabel->setEnabled(m_controller->isHistoryEnabled());
        m_historyRetentionCombo->setEnabled(m_controller->isHistoryEnabled());

        m_updating = false;
        updateDrainingState();
    }

    void updateDrainingState()
    {
        if (!m_controller || !m_controller->isDraining()) {
            m_drainingLabel->hide();
            return;
        }

        const int active = m_controller->activeCount();
        const int effective = m_controller->effectiveConcurrencyLimit();
        m_drainingLabel->setText(transferQueueDrainingText(active, effective));
        m_drainingLabel->show();
    }

    void loadSettings(QSettings &settings)
    {
        const TransferQueueConfig config = TransferQueueSettings::load(settings);
        applyConfig(config);
    }

    void saveSettings(QSettings &settings) const
    {
        TransferQueueConfig config;
        config.mode = m_sequentialRadio->isChecked()
            ? TransferQueueMode::Sequential
            : TransferQueueMode::Parallel;
        config.parallelLimit = m_limitSpin->value();
        config.bandwidthLimitEnabled = m_bandwidthCheck->isChecked();
        config.bandwidthLimitValue = m_bandwidthSpin->value();
        config.bandwidthLimitUnit = static_cast<BandwidthUnit>(m_bandwidthUnitCombo->currentData().toInt());
        config.bandwidthLimitBytesPerSec = bandwidthToBytesPerSec(config.bandwidthLimitValue, config.bandwidthLimitUnit);
        config.closeWindowOnComplete = m_closeOnCompleteCheckBox->isChecked();
        config.desktopNotificationsEnabled = m_desktopNotificationsCheckBox->isChecked();
        config.historyEnabled = m_historyEnabledCheckBox->isChecked();
        config.historyMaxRecords = m_historyRetentionCombo->currentData().toInt();
        TransferQueueSettings::save(settings, config);
    }

    void applyConfig(const TransferQueueConfig &config)
    {
        m_updating = true;
        if (config.mode == TransferQueueMode::Sequential) {
            m_sequentialRadio->setChecked(true);
            m_limitSpin->setEnabled(false);
            m_limitLabel->setEnabled(false);
        } else {
            m_parallelRadio->setChecked(true);
            m_limitSpin->setEnabled(true);
            m_limitLabel->setEnabled(true);
        }
        m_limitSpin->setValue(config.parallelLimit);

        m_bandwidthCheck->setChecked(config.bandwidthLimitEnabled);
        m_bandwidthSpin->setEnabled(config.bandwidthLimitEnabled);
        m_bandwidthUnitCombo->setEnabled(config.bandwidthLimitEnabled);
        m_bandwidthSpin->setValue(config.bandwidthLimitValue > 0 ? config.bandwidthLimitValue : 10);
        const int uIdx = m_bandwidthUnitCombo->findData(static_cast<int>(config.bandwidthLimitUnit));
        if (uIdx >= 0) m_bandwidthUnitCombo->setCurrentIndex(uIdx);

        m_closeOnCompleteCheckBox->setChecked(config.closeWindowOnComplete);
        m_desktopNotificationsCheckBox->setChecked(config.desktopNotificationsEnabled);

        m_historyEnabledCheckBox->setChecked(config.historyEnabled);
        const int histIdx = m_historyRetentionCombo->findData(config.historyMaxRecords);
        if (histIdx >= 0) m_historyRetentionCombo->setCurrentIndex(histIdx);
        m_historyRetentionLabel->setEnabled(config.historyEnabled);
        m_historyRetentionCombo->setEnabled(config.historyEnabled);

        m_updating = false;

        if (m_controller) {
            m_controller->applyConfig(config);
        }
        updateDrainingState();
    }

    void setIsolatedSettings(std::shared_ptr<QSettings> settings)
    {
        m_isolatedSettings = settings;
    }

    QRadioButton *sequentialRadio() const { return m_sequentialRadio; }
    QRadioButton *parallelRadio() const { return m_parallelRadio; }
    QSpinBox *limitSpin() const { return m_limitSpin; }
    QCheckBox *bandwidthCheck() const { return m_bandwidthCheck; }
    QSpinBox *bandwidthSpin() const { return m_bandwidthSpin; }
    QComboBox *bandwidthUnitCombo() const { return m_bandwidthUnitCombo; }
    QLabel *bandwidthInfoLabel() const { return m_bandwidthInfoLabel; }
    QCheckBox *closeOnCompleteCheckBox() const { return m_closeOnCompleteCheckBox; }
    QCheckBox *desktopNotificationsCheckBox() const { return m_desktopNotificationsCheckBox; }
    QCheckBox *historyEnabledCheckBox() const { return m_historyEnabledCheckBox; }
    QLabel *historyRetentionLabel() const { return m_historyRetentionLabel; }
    QComboBox *historyRetentionCombo() const { return m_historyRetentionCombo; }
    QPushButton *clearHistoryButton() const { return m_clearHistoryButton; }
    QLabel *drainingLabel() const { return m_drainingLabel; }
    QGroupBox *groupBox() const { return m_groupBox; }

    QSize sizeHint() const override
    {
        int h = 142;
        if (m_drainingLabel && m_drainingLabel->isVisible()) {
            h += m_drainingLabel->sizeHint().height() + 6;
        }
        return QSize(440, h);
    }

    QSize minimumSizeHint() const override
    {
        int h = 108;
        if (m_drainingLabel && m_drainingLabel->isVisible()) {
            h += m_drainingLabel->sizeHint().height() + 6;
        }
        return QSize(300, h);
    }

Q_SIGNALS:
    void closeWindowOnCompleteToggled(bool checked);

private:
    // Until production integration the queue settings are written only to an
    // explicitly injected store (tests) or the isolated demo store. The real
    // user profile is never touched by Stage 3.
    void persistSettings()
    {
        if (m_isolatedSettings) {
            saveSettings(*m_isolatedSettings);
            m_isolatedSettings->sync();
        } else if (isTransferQueueDemoMode() && !isTransferQueueProductionDispatchConnected()) {
            auto store = transferQueueDemoSettingsStore();
            if (store) {
                saveSettings(*store);
                store->sync();
            }
        } else {
            QSettings store;
            saveSettings(store);
            store.sync();
        }
    }

    QPointer<TransferQueueController> m_controller;
    std::shared_ptr<QSettings> m_isolatedSettings;
    QGroupBox *m_groupBox = nullptr;
    QRadioButton *m_sequentialRadio = nullptr;
    QRadioButton *m_parallelRadio = nullptr;
    QButtonGroup *m_modeGroup = nullptr;
    QLabel *m_limitLabel = nullptr;
    QSpinBox *m_limitSpin = nullptr;
    QCheckBox *m_bandwidthCheck = nullptr;
    QSpinBox *m_bandwidthSpin = nullptr;
    QComboBox *m_bandwidthUnitCombo = nullptr;
    QLabel *m_bandwidthInfoLabel = nullptr;
    QCheckBox *m_closeOnCompleteCheckBox = nullptr;
    QCheckBox *m_desktopNotificationsCheckBox = nullptr;
    QCheckBox *m_historyEnabledCheckBox = nullptr;
    QLabel *m_historyRetentionLabel = nullptr;
    QComboBox *m_historyRetentionCombo = nullptr;
    QPushButton *m_clearHistoryButton = nullptr;
    QLabel *m_drainingLabel = nullptr;
    bool m_updating = false;
};

// One compact queue row. Layout (no word wrapping anywhere, so no
// height-for-width gaps):
//   [icon] Title (elided) ............ [#N in queue] [priority] [STATE] [x]
//          Waiting reason (only for Queued, elided)
//          From: /source/path (middle-elided)
//          To:   /destination/path (middle-elided)
//          [progress ......................] 42%   (active states only)
class TransferQueueTaskItemWidget final : public QFrame
{
    Q_OBJECT

public:
    explicit TransferQueueTaskItemWidget(
        const TransferTaskSnapshot &snapshot,
        TransferQueueController *controller,
        QWidget *parent = nullptr)
        : QFrame(parent)
        , m_id(snapshot.id)
        , m_controller(controller)
    {
        setObjectName(QStringLiteral("transferQueueTaskItem"));
        setProperty("taskId", QVariant::fromValue(snapshot.id));
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Plain);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto *mainLayout = new QHBoxLayout(this);
        mainLayout->setContentsMargins(8, 6, 8, 6);
        mainLayout->setSpacing(8);

        m_iconLabel = new QLabel(this);
        m_iconLabel->setObjectName(QStringLiteral("transferQueueTaskIcon"));
        m_iconLabel->setFixedSize(24, 24);
        mainLayout->addWidget(m_iconLabel, 0, Qt::AlignTop);

        auto *contentLayout = new QVBoxLayout;
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(2);

        auto *topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(6);

        m_titleLabel = new TransferQueueElidedLabel(this, Qt::ElideMiddle);
        m_titleLabel->setObjectName(QStringLiteral("transferQueueTaskTitle"));
        QFont titleFont = m_titleLabel->font();
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);
        topRow->addWidget(m_titleLabel, 1);

        const QString badgeStyle = QStringLiteral(
            "QLabel { background-color: rgba(128, 128, 128, 0.18); "
            "border-radius: 4px; padding: 1px 6px; }");

        m_queuePosBadge = new QLabel(this);
        m_queuePosBadge->setObjectName(QStringLiteral("transferQueueTaskQueuePosBadge"));
        m_queuePosBadge->setStyleSheet(badgeStyle);
        topRow->addWidget(m_queuePosBadge);

        m_moveUpButton = new QToolButton(this);
        m_moveUpButton->setObjectName(QStringLiteral("transferQueueTaskMoveUpButton"));
        m_moveUpButton->setIcon(themedIcon(QStringLiteral("go-up"), QStringLiteral("arrow-up")));
        m_moveUpButton->setToolTip(trLocal("Przesuń wyżej w kolejce", "Move up in queue"));
        m_moveUpButton->setAutoRaise(true);
        m_moveUpButton->setFixedSize(22, 22);
        connect(m_moveUpButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                m_controller->moveTaskUp(m_id);
            }
        });
        topRow->addWidget(m_moveUpButton);

        m_moveDownButton = new QToolButton(this);
        m_moveDownButton->setObjectName(QStringLiteral("transferQueueTaskMoveDownButton"));
        m_moveDownButton->setIcon(themedIcon(QStringLiteral("go-down"), QStringLiteral("arrow-down")));
        m_moveDownButton->setToolTip(trLocal("Przesuń niżej w kolejce", "Move down in queue"));
        m_moveDownButton->setAutoRaise(true);
        m_moveDownButton->setFixedSize(22, 22);
        connect(m_moveDownButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                m_controller->moveTaskDown(m_id);
            }
        });
        topRow->addWidget(m_moveDownButton);

        m_priorityCombo = new QComboBox(this);
        m_priorityCombo->setObjectName(QStringLiteral("transferQueueTaskPriorityCombo"));
        m_priorityCombo->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_priorityCombo->setFixedWidth(96);
        m_priorityCombo->setToolTip(trLocal("Priorytet zadania", "Task priority"));
        m_priorityCombo->addItem(priorityText(TransferPriority::High), static_cast<int>(TransferPriority::High));
        m_priorityCombo->addItem(priorityText(TransferPriority::Normal), static_cast<int>(TransferPriority::Normal));
        m_priorityCombo->addItem(priorityText(TransferPriority::Low), static_cast<int>(TransferPriority::Low));
        connect(m_priorityCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
            if (!m_updatingPriority && m_controller && idx >= 0) {
                const auto prio = static_cast<TransferPriority>(m_priorityCombo->itemData(idx).toInt());
                m_controller->setTaskPriority(m_id, prio);
            }
        });
        topRow->addWidget(m_priorityCombo);

        m_priorityBadge = new QLabel(this);
        m_priorityBadge->setObjectName(QStringLiteral("transferQueueTaskPriorityBadge"));
        m_priorityBadge->setStyleSheet(badgeStyle);
        topRow->addWidget(m_priorityBadge);

        m_throttlingBadge = new QLabel(this);
        m_throttlingBadge->setObjectName(QStringLiteral("transferQueueTaskThrottlingBadge"));
        m_throttlingBadge->setStyleSheet(badgeStyle);
        m_throttlingBadge->hide();
        topRow->addWidget(m_throttlingBadge);

        m_stateBadge = new QLabel(this);
        m_stateBadge->setObjectName(QStringLiteral("transferQueueTaskStateBadge"));
        QFont stateFont = m_stateBadge->font();
        stateFont.setBold(true);
        m_stateBadge->setFont(stateFont);
        topRow->addWidget(m_stateBadge);

        m_cancelButton = new QToolButton(this);
        m_cancelButton->setObjectName(QStringLiteral("transferQueueTaskCancelButton"));
        m_cancelButton->setIcon(themedIcon(QStringLiteral("process-stop")));
        m_cancelButton->setToolTip(trLocal("Anuluj to zadanie z kolejki", "Cancel this queued task"));
        m_cancelButton->setAutoRaise(true);
        connect(m_cancelButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                m_controller->cancelTask(m_id);
            }
        });
        topRow->addWidget(m_cancelButton);

        m_removeButton = new QToolButton(this);
        m_removeButton->setObjectName(QStringLiteral("transferQueueTaskRemoveButton"));
        m_removeButton->setIcon(themedIcon(QStringLiteral("edit-delete"), QStringLiteral("list-remove")));
        m_removeButton->setToolTip(trLocal("Usuń z historii", "Remove from history"));
        m_removeButton->setAutoRaise(true);
        connect(m_removeButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                m_controller->removeFinishedTask(m_id);
            }
        });
        topRow->addWidget(m_removeButton);

        m_retryButton = new QToolButton(this);
        m_retryButton->setObjectName(QStringLiteral("transferQueueTaskRetryButton"));
        m_retryButton->setIcon(themedIcon(QStringLiteral("view-refresh"), QStringLiteral("edit-redo")));
        m_retryButton->setText(trLocal("Ponów", "Retry"));
        m_retryButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_retryButton->setAutoRaise(true);
        connect(m_retryButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                m_retryButton->setEnabled(false);
                m_retryButton->setText(trLocal("Ponawianie…", "Retrying…"));
                m_controller->retryTask(m_id);
            }
        });
        topRow->addWidget(m_retryButton);
        contentLayout->addLayout(topRow);

        m_waitReasonLabel = new TransferQueueElidedLabel(this, Qt::ElideRight);
        m_waitReasonLabel->setObjectName(QStringLiteral("transferQueueTaskWaitReasonLabel"));
        QFont waitFont = m_waitReasonLabel->font();
        waitFont.setItalic(true);
        m_waitReasonLabel->setFont(waitFont);
        contentLayout->addWidget(m_waitReasonLabel);

        m_pathsLabel = new TransferQueueElidedLabel(this, Qt::ElideMiddle);
        m_pathsLabel->setObjectName(QStringLiteral("transferQueueTaskPathsLabel"));
        m_pathsLabel->setForegroundRole(QPalette::PlaceholderText);
        contentLayout->addWidget(m_pathsLabel);

        m_destinationLabel = new TransferQueueElidedLabel(this, Qt::ElideMiddle);
        m_destinationLabel->setObjectName(QStringLiteral("transferQueueTaskDestinationLabel"));
        m_destinationLabel->setForegroundRole(QPalette::PlaceholderText);
        contentLayout->addWidget(m_destinationLabel);

        auto *progressRow = new QHBoxLayout;
        progressRow->setContentsMargins(0, 2, 0, 0);
        progressRow->setSpacing(8);

        m_progressBar = new QProgressBar(this);
        m_progressBar->setObjectName(QStringLiteral("transferQueueTaskProgressBar"));
        m_progressBar->setRange(0, 100);
        m_progressBar->setTextVisible(false);
        m_progressBar->setMaximumHeight(10);
        progressRow->addWidget(m_progressBar, 1);

        m_percentLabel = new QLabel(this);
        m_percentLabel->setObjectName(QStringLiteral("transferQueueTaskPercentLabel"));
        m_percentLabel->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("100%")) + 4);
        m_percentLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        progressRow->addWidget(m_percentLabel);

        contentLayout->addLayout(progressRow);
        mainLayout->addLayout(contentLayout, 1);

        if (m_controller) {
            connect(m_controller, &TransferQueueController::retryEligibilityChanged,
                    this, [this](quint64 taskId, bool eligible, const QString &reason) {
                        if (taskId == m_id && m_retryButton) {
                            const auto snap = m_controller->taskSnapshot(m_id);
                            if (snap && snap->retriedAsTaskId != 0) {
                                m_retryButton->setEnabled(false);
                                m_retryButton->setToolTip(trLocal("Zadanie zostało już ponowione (jako zadanie #%1)",
                                                                  "Task has already been retried (as task #%1)").arg(snap->retriedAsTaskId));
                            } else if (eligible) {
                                m_retryButton->setEnabled(true);
                                m_retryButton->setText(trLocal("Ponów", "Retry"));
                                m_retryButton->setToolTip(trLocal("Ponów tę nieudaną operację", "Retry this failed operation"));
                            } else {
                                m_retryButton->setEnabled(false);
                                m_retryButton->setText(trLocal("Ponów", "Retry"));
                                const QString r = !reason.isEmpty()
                                    ? reason
                                    : trLocal("Ponowienie tej operacji nie jest możliwe", "Retrying this operation is not possible");
                                m_retryButton->setToolTip(trLocal("Ponowienie niedostępne: %1", "Retry unavailable: %1").arg(r));
                            }
                        }
                    });
        }

        updateFromSnapshot(snapshot);
    }

    quint64 taskId() const { return m_id; }

    void updateFromSnapshot(const TransferTaskSnapshot &snapshot)
    {
        m_id = snapshot.id;

        const QString title = snapshot.title.isEmpty()
            ? (snapshot.sources.isEmpty() ? trLocal("Transfer", "Transfer")
                                         : snapshot.sources.first().fileName())
            : snapshot.title;
        m_titleLabel->setFullText(title);
        m_titleLabel->setToolTip(title);

        const QString iconName = (snapshot.type == TransferOperationType::Move)
            ? QStringLiteral("edit-cut")
            : QStringLiteral("edit-copy");
        m_iconLabel->setPixmap(themedIcon(iconName, QStringLiteral("folder-sync")).pixmap(22, 22));

        m_stateBadge->setText(stateText(snapshot.state));
        updateStateBadgeStyle(snapshot.state);

        // Queue position and Stage 4 reordering/priority controls
        const bool isQueued = (snapshot.state == TransferTaskState::Queued);
        m_moveUpButton->setVisible(isQueued);
        m_moveDownButton->setVisible(isQueued);
        m_priorityCombo->setVisible(isQueued);

        if (isQueued) {
            m_moveUpButton->setEnabled(m_controller ? m_controller->canMoveTaskUp(m_id) : false);
            m_moveDownButton->setEnabled(m_controller ? m_controller->canMoveTaskDown(m_id) : false);
            m_moveUpButton->setToolTip(trLocal("Przesuń wyżej w kolejce", "Move up in queue"));
            m_moveDownButton->setToolTip(trLocal("Przesuń niżej w kolejce", "Move down in queue"));

            m_updatingPriority = true;
            m_priorityCombo->setItemText(0, priorityText(TransferPriority::High));
            m_priorityCombo->setItemText(1, priorityText(TransferPriority::Normal));
            m_priorityCombo->setItemText(2, priorityText(TransferPriority::Low));
            const int comboIdx = m_priorityCombo->findData(static_cast<int>(snapshot.priority));
            if (comboIdx >= 0 && m_priorityCombo->currentIndex() != comboIdx) {
                m_priorityCombo->setCurrentIndex(comboIdx);
            }
            m_updatingPriority = false;

            if (snapshot.queuePosition > 0) {
                m_queuePosBadge->setText(
                    isPolish() ? QStringLiteral("#%1 w kolejce").arg(snapshot.queuePosition)
                               : QStringLiteral("#%1 in queue").arg(snapshot.queuePosition));
                m_queuePosBadge->show();
            } else {
                m_queuePosBadge->hide();
            }
        } else {
            m_queuePosBadge->hide();
        }

        m_priorityBadge->setText(priorityText(snapshot.priority));
        m_priorityBadge->setToolTip(trLocal("Priorytet", "Priority"));
        m_priorityBadge->setVisible(!isQueued && !snapshot.terminal);

        // Stage 6 Throttling badge
        if (m_controller && m_controller->isBandwidthLimitEnabled() && !snapshot.terminal) {
            if (snapshot.supportsThrottling) {
                m_throttlingBadge->setText(trLocal("Limitowany", "Throttled"));
                m_throttlingBadge->setToolTip(trLocal("Prędkość ograniczana przez limiter", "Speed limited by bandwidth limiter"));
            } else {
                m_throttlingBadge->setText(trLocal("Brak limitu", "No limit"));
                m_throttlingBadge->setToolTip(trLocal("To zadanie nie obsługuje limitu prędkości", "This task does not support speed limiting"));
            }
            m_throttlingBadge->show();
        } else {
            m_throttlingBadge->hide();
        }

        if (snapshot.state == TransferTaskState::Queued && snapshot.waitReason != TransferWaitReason::None) {
            m_waitReasonLabel->setFullText(waitReasonText(snapshot.waitReason));
            m_waitReasonLabel->setToolTip(waitReasonTooltip(snapshot.waitReason, snapshot.waitBlockerId));
            m_waitReasonLabel->show();
        } else if (snapshot.state == TransferTaskState::Failed && !snapshot.errorText.isEmpty()) {
            m_waitReasonLabel->setFullText(snapshot.errorText);
            m_waitReasonLabel->setToolTip(snapshot.errorText);
            m_waitReasonLabel->show();
        } else {
            m_waitReasonLabel->hide();
        }

        const QString fullSources = formatFullSourcesText(snapshot.sources);
        const QString dstText = displayUrl(snapshot.destination);
        const QString srcCaption = isPolish() ? QStringLiteral("Źródło") : QStringLiteral("Source");
        const QString dstCaption = isPolish() ? QStringLiteral("Cel") : QStringLiteral("Target");
        const QString pathTooltip = formatPathTooltip(srcCaption, fullSources, dstCaption, dstText);
        m_pathsLabel->setFullText(
            (isPolish() ? QStringLiteral("Z: %1") : QStringLiteral("From: %1"))
                .arg(formatSourcesText(snapshot.sources)));
        m_pathsLabel->setToolTip(pathTooltip);
        m_destinationLabel->setFullText(
            (isPolish() ? QStringLiteral("Do: %1") : QStringLiteral("To: %1")).arg(dstText));
        m_destinationLabel->setToolTip(pathTooltip);
        m_destinationLabel->setVisible(!dstText.isEmpty());

        const bool showProgress = (snapshot.state == TransferTaskState::Running
                                   || snapshot.state == TransferTaskState::Paused
                                   || snapshot.state == TransferTaskState::Cancelling);
        m_progressBar->setVisible(showProgress);
        m_percentLabel->setVisible(showProgress);
        if (showProgress) {
            m_progressBar->setValue(snapshot.percent);
            m_percentLabel->setText(QStringLiteral("%1%").arg(snapshot.percent));
        }

        m_cancelButton->setEnabled(snapshot.canCancel && !snapshot.terminal);
        m_cancelButton->setVisible(!snapshot.terminal);
        m_removeButton->setEnabled(snapshot.terminal);
        m_removeButton->setVisible(snapshot.terminal);

        const bool isFailed = (snapshot.state == TransferTaskState::Failed);
        m_retryButton->setVisible(isFailed);
        if (isFailed) {
            if (snapshot.retriedAsTaskId != 0) {
                m_retryButton->setEnabled(false);
                m_retryButton->setToolTip(trLocal("Zadanie zostało już ponowione (jako zadanie #%1)",
                                                  "Task has already been retried (as task #%1)").arg(snapshot.retriedAsTaskId));
            } else if (snapshot.canRetry) {
                m_retryButton->setEnabled(true);
                m_retryButton->setText(trLocal("Ponów", "Retry"));
                m_retryButton->setToolTip(trLocal("Ponów tę nieudaną operację", "Retry this failed operation"));
            } else {
                m_retryButton->setEnabled(false);
                m_retryButton->setText(trLocal("Ponów", "Retry"));
                const QString reason = !snapshot.retryRejectionReason.isEmpty()
                    ? snapshot.retryRejectionReason
                    : trLocal("Ponowienie tej operacji nie jest możliwe", "Retrying this operation is not possible");
                m_retryButton->setToolTip(trLocal("Ponowienie niedostępne: %1", "Retry unavailable: %1").arg(reason));
            }
        }
    }

    void updateProgress(quint64 processed, quint64 total, int percent)
    {
        Q_UNUSED(processed)
        Q_UNUSED(total)
        m_progressBar->setVisible(true);
        m_percentLabel->setVisible(true);
        m_progressBar->setValue(percent);
        m_percentLabel->setText(QStringLiteral("%1%").arg(percent));
    }

    static QString stateText(TransferTaskState state)
    {
        switch (state) {
        case TransferTaskState::Queued: return trLocal("W kolejce", "Queued");
        case TransferTaskState::Running: return trLocal("W toku", "Running");
        case TransferTaskState::Paused: return trLocal("Wstrzymano", "Paused");
        case TransferTaskState::Cancelling: return trLocal("Anulowanie…", "Cancelling…");
        case TransferTaskState::Completed: return trLocal("Zakończono", "Completed");
        case TransferTaskState::Cancelled: return trLocal("Anulowano", "Cancelled");
        case TransferTaskState::Failed: return trLocal("Błąd", "Failed");
        }
        return {};
    }

    static QString priorityText(TransferPriority priority)
    {
        switch (priority) {
        case TransferPriority::High: return trLocal("Wysoki", "High");
        case TransferPriority::Normal: return trLocal("Normalny", "Normal");
        case TransferPriority::Low: return trLocal("Niski", "Low");
        }
        return {};
    }

    static QString waitReasonText(TransferWaitReason reason)
    {
        switch (reason) {
        case TransferWaitReason::SlotLimit:
            return trLocal("Oczekuje na wolne miejsce (limit jednoczesnych)",
                           "Waiting for a free slot (concurrency limit)");
        case TransferWaitReason::DispatchPaused:
            return trLocal("Uruchamianie zadań wstrzymane", "Task dispatch paused");
        case TransferWaitReason::BlockedByReservation:
            return trLocal("Oczekuje na zakończenie operacji na tej samej ścieżce",
                           "Waiting for an operation on the same path");
        case TransferWaitReason::ExclusiveBarrier:
            return trLocal("Oczekuje na zakończenie operacji wyłącznej",
                           "Waiting for an exclusive operation");
        case TransferWaitReason::ShuttingDown:
            return trLocal("Zamykanie aplikacji", "Application shutting down");
        case TransferWaitReason::None:
            return {};
        }
        return {};
    }

    static QString waitReasonTooltip(TransferWaitReason reason, quint64 blockerId)
    {
        QString base = waitReasonText(reason);
        if (blockerId > 0) {
            base += QStringLiteral(" (#%1)").arg(blockerId);
        }
        return base;
    }

    static QString displayUrl(const QUrl &url)
    {
        if (url.isEmpty()) {
            return {};
        }
        return url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : url.toDisplayString();
    }

    static QString formatSourcesText(const QList<QUrl> &sources)
    {
        if (sources.isEmpty()) return {};
        const QString first = displayUrl(sources.first());
        if (sources.size() == 1) return first;
        return isPolish()
            ? QStringLiteral("%1  (+%2 kolejnych)").arg(first).arg(sources.size() - 1)
            : QStringLiteral("%1  (+%2 more)").arg(first).arg(sources.size() - 1);
    }

    static QString formatFullSourcesText(const QList<QUrl> &sources)
    {
        QStringList paths;
        paths.reserve(sources.size());
        for (const auto &url : sources) {
            paths.push_back(displayUrl(url));
        }
        return paths.join(QLatin1Char('\n'));
    }

    TransferQueueElidedLabel *titleLabel() const { return m_titleLabel; }
    QLabel *stateBadge() const { return m_stateBadge; }
    QLabel *queuePosBadge() const { return m_queuePosBadge; }
    QLabel *priorityBadge() const { return m_priorityBadge; }
    QLabel *throttlingBadge() const { return m_throttlingBadge; }
    QToolButton *moveUpButton() const { return m_moveUpButton; }
    QToolButton *moveDownButton() const { return m_moveDownButton; }
    QComboBox *priorityCombo() const { return m_priorityCombo; }
    TransferQueueElidedLabel *waitReasonLabel() const { return m_waitReasonLabel; }
    TransferQueueElidedLabel *pathsLabel() const { return m_pathsLabel; }
    TransferQueueElidedLabel *destinationLabel() const { return m_destinationLabel; }
    QProgressBar *progressBar() const { return m_progressBar; }
    QToolButton *cancelButton() const { return m_cancelButton; }
    QToolButton *removeButton() const { return m_removeButton; }

private:
    // Translucent tint + palette text colour: the label text keeps the KDE
    // palette contrast in both light and dark schemes.
    void updateStateBadgeStyle(TransferTaskState state)
    {
        QString tint;
        switch (state) {
        case TransferTaskState::Queued: tint = QStringLiteral("52, 152, 219"); break;
        case TransferTaskState::Running: tint = QStringLiteral("46, 204, 113"); break;
        case TransferTaskState::Paused: tint = QStringLiteral("241, 196, 15"); break;
        case TransferTaskState::Cancelling: tint = QStringLiteral("230, 126, 34"); break;
        case TransferTaskState::Completed: tint = QStringLiteral("39, 174, 96"); break;
        case TransferTaskState::Cancelled: tint = QStringLiteral("149, 165, 166"); break;
        case TransferTaskState::Failed: tint = QStringLiteral("231, 76, 60"); break;
        }
        m_stateBadge->setStyleSheet(
            QStringLiteral("QLabel { background-color: rgba(%1, 0.28); "
                           "border: 1px solid rgba(%1, 0.6); border-radius: 4px; "
                           "padding: 1px 6px; }")
                .arg(tint));
    }

    quint64 m_id = 0;
    QPointer<TransferQueueController> m_controller;
    QLabel *m_iconLabel = nullptr;
    TransferQueueElidedLabel *m_titleLabel = nullptr;
    QLabel *m_stateBadge = nullptr;
    QLabel *m_queuePosBadge = nullptr;
    QLabel *m_priorityBadge = nullptr;
    QLabel *m_throttlingBadge = nullptr;
    QToolButton *m_moveUpButton = nullptr;
    QToolButton *m_moveDownButton = nullptr;
    QComboBox *m_priorityCombo = nullptr;
    bool m_updatingPriority = false;
    TransferQueueElidedLabel *m_waitReasonLabel = nullptr;
    TransferQueueElidedLabel *m_pathsLabel = nullptr;
    TransferQueueElidedLabel *m_destinationLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_percentLabel = nullptr;
    QToolButton *m_cancelButton = nullptr;
    QToolButton *m_removeButton = nullptr;
    QToolButton *m_retryButton = nullptr;
};

struct TransferQueueCounts {
    int active = 0;     // Running / Paused / Cancelling
    int queued = 0;
    int completed = 0;
    int cancelled = 0;
    int failed = 0;
};

inline TransferQueueCounts transferQueueCounts(const TransferQueueSnapshot &snapshot)
{
    TransferQueueCounts counts;
    for (const auto &task : snapshot.tasks) {
        switch (task.state) {
        case TransferTaskState::Running:
        case TransferTaskState::Paused:
        case TransferTaskState::Cancelling:
            ++counts.active;
            break;
        case TransferTaskState::Queued:
            ++counts.queued;
            break;
        case TransferTaskState::Completed:
            ++counts.completed;
            break;
        case TransferTaskState::Cancelled:
            ++counts.cancelled;
            break;
        case TransferTaskState::Failed:
            ++counts.failed;
            break;
        }
    }
    return counts;
}

// Queue task list with its own scroll area. Rows are kept in scheduler
// (submission) order and packed at the top: no stretch between rows.
class TransferQueueViewWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TransferQueueViewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("transferQueueViewWidget"));

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(6);

        auto *headerContainer = new QWidget(this);
        headerContainer->setObjectName(QStringLiteral("transferQueueHeaderContainer"));
        headerContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *headerLayout = new QVBoxLayout(headerContainer);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(5);

        auto *row1 = new QHBoxLayout;
        row1->setContentsMargins(0, 0, 0, 0);
        row1->setSpacing(8);

        m_titleLabel = new QLabel(trLocal("Zadania", "Tasks"), headerContainer);
        m_titleLabel->setObjectName(QStringLiteral("transferQueueViewTitle"));
        QFont headerFont = m_titleLabel->font();
        headerFont.setBold(true);
        m_titleLabel->setFont(headerFont);
        m_titleLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        row1->addWidget(m_titleLabel);
        row1->addStretch(1);

        m_demoButton = new QToolButton(headerContainer);
        m_demoButton->setObjectName(QStringLiteral("transferQueueDemoButton"));
        m_demoButton->setIcon(themedIcon(QStringLiteral("list-add"), QStringLiteral("view-task")));
        m_demoButton->setText(trLocal("Dodaj zadania demo", "Add demo tasks"));
        m_demoButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_demoButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_demoButton->setToolTip(
            trLocal("Dodaje symulowane zadania (nie dotyka żadnych plików)",
                    "Adds simulated tasks (does not touch any files)"));
        m_demoButton->setVisible(isTransferQueueDemoMode());
        connect(m_demoButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                enqueueDemoTasks(m_controller);
            }
        });
        row1->addWidget(m_demoButton);
        headerLayout->addLayout(row1);

        // Stage 5: Global Queue Controls
        auto *globalControlsRow = new QHBoxLayout;
        globalControlsRow->setContentsMargins(0, 0, 0, 0);
        globalControlsRow->setSpacing(6);

        m_pauseAllButton = new QToolButton(headerContainer);
        m_pauseAllButton->setObjectName(QStringLiteral("transferQueuePauseAllButton"));
        m_pauseAllButton->setIcon(themedIcon(QStringLiteral("media-playback-pause")));
        m_pauseAllButton->setText(trLocal("Wstrzymaj wszystkie", "Pause All"));
        m_pauseAllButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_pauseAllButton->setToolTip(trLocal(
            "Wstrzymuje aktywne transfery i blokuje uruchamianie kolejnych zadań",
            "Suspends active transfers and blocks launching further tasks"));
        connect(m_pauseAllButton, &QToolButton::clicked, this, [this] {
            if (m_controller) m_controller->pauseAll();
        });
        globalControlsRow->addWidget(m_pauseAllButton);

        m_resumeAllButton = new QToolButton(headerContainer);
        m_resumeAllButton->setObjectName(QStringLiteral("transferQueueResumeAllButton"));
        m_resumeAllButton->setIcon(themedIcon(QStringLiteral("media-playback-start")));
        m_resumeAllButton->setText(trLocal("Wznów wszystkie", "Resume All"));
        m_resumeAllButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_resumeAllButton->setToolTip(trLocal(
            "Wznawia wstrzymane zadania i odblokowuje uruchamianie kolejnych",
            "Resumes paused transfers and unlocks task dispatch"));
        connect(m_resumeAllButton, &QToolButton::clicked, this, [this] {
            if (m_controller) m_controller->resumeAll();
        });
        globalControlsRow->addWidget(m_resumeAllButton);

        m_cancelAllButton = new QToolButton(headerContainer);
        m_cancelAllButton->setObjectName(QStringLiteral("transferQueueCancelAllButton"));
        m_cancelAllButton->setIcon(themedIcon(QStringLiteral("process-stop"), QStringLiteral("dialog-cancel")));
        m_cancelAllButton->setText(trLocal("Anuluj wszystkie", "Cancel All"));
        m_cancelAllButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_cancelAllButton->setToolTip(trLocal(
            "Anuluje wszystkie oczekujące i aktywne zadania w kolejce",
            "Cancels all queued and active tasks in the queue"));
        connect(m_cancelAllButton, &QToolButton::clicked, this, [this] {
            promptCancelAll();
        });
        globalControlsRow->addWidget(m_cancelAllButton);

        m_clearHistoryButton = new QToolButton(headerContainer);
        m_clearHistoryButton->setObjectName(QStringLiteral("transferQueueClearHistoryButton"));
        m_clearHistoryButton->setIcon(themedIcon(QStringLiteral("edit-clear-history"), QStringLiteral("edit-clear")));
        m_clearHistoryButton->setText(trLocal("Wyczyść historię", "Clear History"));
        m_clearHistoryButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_clearHistoryButton->setToolTip(trLocal(
            "Usuwa zakończone i anulowane zadania z widoku historii",
            "Removes completed and cancelled tasks from the history view"));
        connect(m_clearHistoryButton, &QToolButton::clicked, this, [this] {
            if (m_controller) {
                if (promptClearHistoryConfirmation(this)) {
                    m_controller->clearHistory(true);
                }
            }
        });
        globalControlsRow->addWidget(m_clearHistoryButton);

        globalControlsRow->addStretch(1);
        headerLayout->addLayout(globalControlsRow);

        // Stage 5: Global Status Banner
        m_globalStatusBanner = new QFrame(headerContainer);
        m_globalStatusBanner->setObjectName(QStringLiteral("transferQueueGlobalStatusBanner"));
        m_globalStatusBanner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto *bannerLayout = new QHBoxLayout(m_globalStatusBanner);
        bannerLayout->setContentsMargins(8, 4, 8, 4);
        bannerLayout->setSpacing(6);
        m_globalStatusLabel = new QLabel(m_globalStatusBanner);
        m_globalStatusLabel->setObjectName(QStringLiteral("transferQueueGlobalStatusLabel"));
        m_globalStatusLabel->setWordWrap(true);
        bannerLayout->addWidget(m_globalStatusLabel);
        m_globalStatusBanner->hide();
        headerLayout->addWidget(m_globalStatusBanner);

        m_countLabel = new QLabel(headerContainer);
        m_countLabel->setObjectName(QStringLiteral("transferQueueViewCount"));
        m_countLabel->setWordWrap(true);
        m_countLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_countLabel->setMinimumHeight(m_countLabel->fontMetrics().height());
        m_countLabel->setForegroundRole(QPalette::PlaceholderText);
        headerLayout->addWidget(m_countLabel);

        mainLayout->addWidget(headerContainer);

        m_scrollArea = new QScrollArea(this);
        m_scrollArea->setObjectName(QStringLiteral("transferQueueScrollArea"));
        m_scrollArea->setWidgetResizable(true);
        m_scrollArea->setFrameShape(QFrame::NoFrame);
        m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scrollArea->setMinimumHeight(60);

        m_tasksContainer = new QWidget(m_scrollArea);
        m_tasksContainer->setObjectName(QStringLiteral("transferQueueTasksContainer"));
        m_tasksLayout = new QVBoxLayout(m_tasksContainer);
        m_tasksLayout->setContentsMargins(0, 0, 0, 0);
        m_tasksLayout->setSpacing(4);

        m_emptyLabel = new QLabel(
            trLocal("Kolejka transferów jest pusta.", "Transfer queue is empty."),
            m_tasksContainer);
        m_emptyLabel->setObjectName(QStringLiteral("transferQueueEmptyLabel"));
        m_emptyLabel->setAlignment(Qt::AlignCenter);
        m_emptyLabel->setMinimumHeight(60);
        m_emptyLabel->setForegroundRole(QPalette::PlaceholderText);
        m_tasksLayout->addWidget(m_emptyLabel);
        m_tasksLayout->addStretch(1);

        m_scrollArea->setWidget(m_tasksContainer);
        mainLayout->addWidget(m_scrollArea, 1);
    }

    ~TransferQueueViewWidget() override
    {
        for (const QPointer<TransferQueueTaskItemWidget> &retired : std::as_const(m_retiredItemWidgets)) {
            if (retired) {
                delete retired.data();
            }
        }
        m_retiredItemWidgets.clear();
    }

    void setController(TransferQueueController *controller)
    {
        if (m_controller == controller) {
            return;
        }

        if (m_controller) {
            disconnect(m_controller, nullptr, this, nullptr);
        }
        m_controller = controller;
        if (m_controller) {
            connect(m_controller, &TransferQueueController::queueChanged, this, &TransferQueueViewWidget::rebuildTasks);
            connect(m_controller, &TransferQueueController::taskStateChanged, this, [this](quint64, TransferTaskState, TransferTaskState) {
                rebuildTasks();
            });
            connect(m_controller, &TransferQueueController::taskFinished, this, [this](quint64, TransferTaskState) {
                rebuildTasks();
            });
            connect(m_controller, &TransferQueueController::taskPriorityChanged, this, [this](quint64, TransferPriority) {
                rebuildTasks();
            });
            connect(m_controller, &TransferQueueController::taskOrderChanged, this, [this](quint64) {
                rebuildTasks();
            });
            connect(m_controller, &TransferQueueController::globalControlStateChanged, this, [this](TransferQueueGlobalControlState) {
                updateGlobalControls();
            });
            connect(m_controller, &TransferQueueController::cancelAllCompleted, this, [this] {
                updateGlobalControls();
            });
            connect(m_controller, &TransferQueueController::bandwidthLimitChanged, this, [this](bool, quint64) {
                rebuildTasks();
            });
            connect(m_controller, &TransferQueueController::taskProgressChanged, this,
                    &TransferQueueViewWidget::onTaskProgressChanged);
            rebuildTasks();
        }
    }

    TransferQueueController *controller() const { return m_controller; }

    void setDemoButtonVisible(bool visible)
    {
        m_demoButton->setVisible(visible);
    }

    static QString countsText(const TransferQueueCounts &counts)
    {
        QString text = isPolish()
            ? QStringLiteral("Aktywne: %1 • Oczekujące: %2 • Zakończone: %3")
                  .arg(counts.active).arg(counts.queued).arg(counts.completed)
            : QStringLiteral("Active: %1 • Queued: %2 • Completed: %3")
                  .arg(counts.active).arg(counts.queued).arg(counts.completed);
        if (counts.cancelled > 0) {
            text += isPolish() ? QStringLiteral(" • Anulowane: %1").arg(counts.cancelled)
                               : QStringLiteral(" • Cancelled: %1").arg(counts.cancelled);
        }
        if (counts.failed > 0) {
            text += isPolish() ? QStringLiteral(" • Błędy: %1").arg(counts.failed)
                               : QStringLiteral(" • Failed: %1").arg(counts.failed);
        }
        return text;
    }

    void rebuildTasks()
    {
        if (!m_controller) {
            return;
        }

        const TransferQueueSnapshot snap = m_controller->snapshot();
        const TransferQueueCounts counts = transferQueueCounts(snap);
        const QString summary = countsText(counts);
        m_countLabel->setText(summary);
        m_countLabel->setToolTip(summary);

        const bool hasTasks = !snap.tasks.isEmpty();
        m_emptyLabel->setVisible(!hasTasks);

        m_retiredItemWidgets.erase(
            std::remove_if(m_retiredItemWidgets.begin(), m_retiredItemWidgets.end(),
                           [](const QPointer<TransferQueueTaskItemWidget> &w) { return w.isNull(); }),
            m_retiredItemWidgets.end());

        QSet<quint64> currentIds;
        for (const auto &taskSnap : snap.tasks) {
            currentIds.insert(taskSnap.id);
        }

        for (auto it = m_itemWidgets.begin(); it != m_itemWidgets.end();) {
            if (!currentIds.contains(it.key())) {
                if (TransferQueueTaskItemWidget *w = it.value()) {
                    w->hide();
                    w->setObjectName(QString());
                    w->setParent(nullptr);
                    m_retiredItemWidgets.push_back(w);
                    w->deleteLater();
                }
                it = m_itemWidgets.erase(it);
            } else {
                ++it;
            }
        }

        const int scrollPos = m_scrollArea->verticalScrollBar()->value();

        // Visual presentation order:
        // 1. Active (Running / Paused / Cancelling)
        // 2. Queued (by queue position)
        // 3. Terminal (Completed / Cancelled / Failed)
        auto visualRank = [](TransferTaskState state) -> int {
            switch (state) {
            case TransferTaskState::Running:
            case TransferTaskState::Paused:
            case TransferTaskState::Cancelling:
                return 0;
            case TransferTaskState::Queued:
                return 1;
            case TransferTaskState::Completed:
            case TransferTaskState::Cancelled:
            case TransferTaskState::Failed:
                return 2;
            }
            return 3;
        };

        QList<TransferTaskSnapshot> visualTasks = snap.tasks;
        std::stable_sort(visualTasks.begin(), visualTasks.end(),
                         [&visualRank](const TransferTaskSnapshot &a, const TransferTaskSnapshot &b) {
                             const int rankA = visualRank(a.state);
                             const int rankB = visualRank(b.state);
                             if (rankA != rankB) {
                                 return rankA < rankB;
                             }
                             if (a.state == TransferTaskState::Queued && b.state == TransferTaskState::Queued) {
                                 if (a.queuePosition != b.queuePosition) {
                                     return a.queuePosition < b.queuePosition;
                                 }
                             }
                             return a.id < b.id;
                         });

        // Rows occupy layout indices [0, n); the empty label and the trailing
        // stretch always stay after them.
        int index = 0;
        for (const auto &taskSnap : visualTasks) {
            TransferQueueTaskItemWidget *widget = m_itemWidgets.value(taskSnap.id, nullptr);
            if (!widget) {
                widget = new TransferQueueTaskItemWidget(taskSnap, m_controller, m_tasksContainer);
                m_tasksLayout->insertWidget(index, widget);
                m_itemWidgets.insert(taskSnap.id, widget);
            } else {
                widget->updateFromSnapshot(taskSnap);
                if (m_tasksLayout->indexOf(widget) != index) {
                    m_tasksLayout->removeWidget(widget);
                    m_tasksLayout->insertWidget(index, widget);
                }
            }
            ++index;
        }

        m_scrollArea->verticalScrollBar()->setValue(scrollPos);
        updateGlobalControls();
    }

    void promptCancelAll()
    {
        if (!m_controller) return;
        const TransferQueueSnapshot snap = m_controller->snapshot();
        const TransferQueueCounts counts = transferQueueCounts(snap);
        if (counts.active == 0 && counts.queued == 0) return;

        if (m_confirmCancelAll) {
            const QString title = trLocal("Anuluj wszystkie transfery", "Cancel all transfers");
            const QString text = isPolish()
                ? QStringLiteral("Czy na pewno chcesz anulować wszystkie zadania w kolejce?\n\n"
                                 "Liczba zadań objętych anulowaniem:\n"
                                 " • Aktywne: %1\n"
                                 " • Oczekujące: %2\n\n"
                                 "Zadania już zakończone (%3) nie zostaną usunięte.")
                      .arg(counts.active).arg(counts.queued).arg(counts.completed)
                : QStringLiteral("Are you sure you want to cancel all tasks in the queue?\n\n"
                                 "Tasks to be cancelled:\n"
                                 " • Active: %1\n"
                                 " • Queued: %2\n\n"
                                 "Already completed tasks (%3) will not be removed.")
                      .arg(counts.active).arg(counts.queued).arg(counts.completed);

            QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, this);
            auto *yesBtn = box.addButton(trLocal("Anuluj zadania", "Cancel tasks"), QMessageBox::AcceptRole);
            box.addButton(trLocal("Nie anuluj", "Do not cancel"), QMessageBox::RejectRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == static_cast<QAbstractButton *>(yesBtn)) {
                m_controller->cancelAll();
            }
        } else {
            m_controller->cancelAll();
        }
    }

    void updateGlobalControls()
    {
        if (!m_controller) {
            if (m_pauseAllButton) m_pauseAllButton->setEnabled(false);
            if (m_resumeAllButton) m_resumeAllButton->setEnabled(false);
            if (m_cancelAllButton) m_cancelAllButton->setEnabled(false);
            if (m_clearHistoryButton) m_clearHistoryButton->setEnabled(false);
            if (m_globalStatusBanner) m_globalStatusBanner->hide();
            return;
        }

        const TransferQueueSnapshot snap = m_controller->snapshot();
        const TransferQueueCounts counts = transferQueueCounts(snap);
        const TransferQueueGlobalControlState gState = snap.globalState;

        const bool hasPending = (counts.active > 0 || counts.queued > 0);
        const bool isCancelling = (gState == TransferQueueGlobalControlState::CancellingAll);
        const bool isFullyPaused = (gState == TransferQueueGlobalControlState::Paused);
        const bool isAnyPaused = (gState == TransferQueueGlobalControlState::Paused
                                  || gState == TransferQueueGlobalControlState::PartiallyPaused
                                  || gState == TransferQueueGlobalControlState::DispatchPaused);

        if (m_pauseAllButton) {
            m_pauseAllButton->setEnabled(hasPending && !isFullyPaused && !isCancelling);
        }
        if (m_resumeAllButton) {
            m_resumeAllButton->setEnabled(isAnyPaused && !isCancelling);
        }
        if (m_cancelAllButton) {
            m_cancelAllButton->setEnabled(hasPending && !isCancelling);
        }
        if (m_clearHistoryButton) {
            const int finished = counts.completed + counts.cancelled + counts.failed;
            m_clearHistoryButton->setEnabled(finished > 0);
        }

        if (!m_globalStatusBanner || !m_globalStatusLabel) return;

        switch (gState) {
        case TransferQueueGlobalControlState::Paused:
            m_globalStatusLabel->setText(trLocal(
                "Wszystkie transfery wstrzymane (uruchamianie kolejnych zablokowane)",
                "All transfers paused (new task dispatch blocked)"));
            m_globalStatusBanner->setStyleSheet(QStringLiteral(
                "QFrame { background-color: rgba(241, 196, 15, 0.20); "
                "border: 1px solid rgba(241, 196, 15, 0.6); border-radius: 4px; padding: 4px 8px; }"));
            m_globalStatusBanner->show();
            break;
        case TransferQueueGlobalControlState::PartiallyPaused: {
            int runningCount = 0;
            for (const auto &task : snap.tasks) {
                if (task.state == TransferTaskState::Running) ++runningCount;
            }
            m_globalStatusLabel->setText(isPolish()
                ? QStringLiteral("Częściowo wstrzymano — uruchamianie zablokowane, %1 transfer(ów) nadal w toku (brak obsługi pauzy)")
                      .arg(runningCount)
                : QStringLiteral("Partially paused — dispatch blocked, %1 transfer(s) still running (pause unsupported)")
                      .arg(runningCount));
            m_globalStatusBanner->setStyleSheet(QStringLiteral(
                "QFrame { background-color: rgba(230, 126, 34, 0.20); "
                "border: 1px solid rgba(230, 126, 34, 0.6); border-radius: 4px; padding: 4px 8px; }"));
            m_globalStatusBanner->show();
            break;
        }
        case TransferQueueGlobalControlState::DispatchPaused:
            m_globalStatusLabel->setText(trLocal(
                "Uruchamianie kolejnych zadań wstrzymane (aktywne transfery kontynuowane)",
                "Task dispatch paused (active transfers continue)"));
            m_globalStatusBanner->setStyleSheet(QStringLiteral(
                "QFrame { background-color: rgba(52, 152, 219, 0.20); "
                "border: 1px solid rgba(52, 152, 219, 0.6); border-radius: 4px; padding: 4px 8px; }"));
            m_globalStatusBanner->show();
            break;
        case TransferQueueGlobalControlState::CancellingAll:
            m_globalStatusLabel->setText(trLocal(
                "Trwa anulowanie wszystkich zadań…",
                "Cancelling all tasks…"));
            m_globalStatusBanner->setStyleSheet(QStringLiteral(
                "QFrame { background-color: rgba(231, 76, 60, 0.20); "
                "border: 1px solid rgba(231, 76, 60, 0.6); border-radius: 4px; padding: 4px 8px; }"));
            m_globalStatusBanner->show();
            break;
        case TransferQueueGlobalControlState::Normal:
        default:
            m_globalStatusBanner->hide();
            break;
        }
    }

    QList<TransferQueueTaskItemWidget *> taskItemWidgets() const
    {
        return m_itemWidgets.values();
    }

    // Rows in visual (layout) order.
    QList<TransferQueueTaskItemWidget *> orderedTaskItemWidgets() const
    {
        QList<TransferQueueTaskItemWidget *> rows;
        for (int i = 0; i < m_tasksLayout->count(); ++i) {
            if (auto *row = qobject_cast<TransferQueueTaskItemWidget *>(m_tasksLayout->itemAt(i)->widget())) {
                rows.push_back(row);
            }
        }
        return rows;
    }

    TransferQueueTaskItemWidget *taskItemWidget(quint64 id) const
    {
        return m_itemWidgets.value(id, nullptr);
    }

    QLabel *emptyLabel() const { return m_emptyLabel; }
    QLabel *countLabel() const { return m_countLabel; }
    QToolButton *demoButton() const { return m_demoButton; }
    QToolButton *pauseAllButton() const { return m_pauseAllButton; }
    QToolButton *resumeAllButton() const { return m_resumeAllButton; }
    QToolButton *cancelAllButton() const { return m_cancelAllButton; }
    QToolButton *clearHistoryButton() const { return m_clearHistoryButton; }
    QFrame *globalStatusBanner() const { return m_globalStatusBanner; }
    QLabel *globalStatusLabel() const { return m_globalStatusLabel; }
    void setConfirmCancelAll(bool confirm) { m_confirmCancelAll = confirm; }
    QScrollArea *scrollArea() const { return m_scrollArea; }
    QWidget *tasksContainer() const { return m_tasksContainer; }

private:
    void onTaskProgressChanged(quint64 id, quint64 processed, quint64 total, int percent)
    {
        TransferQueueTaskItemWidget *widget = m_itemWidgets.value(id, nullptr);
        if (widget) {
            widget->updateProgress(processed, total, percent);
        }
    }

    QPointer<TransferQueueController> m_controller;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    QToolButton *m_demoButton = nullptr;
    QToolButton *m_pauseAllButton = nullptr;
    QToolButton *m_resumeAllButton = nullptr;
    QToolButton *m_cancelAllButton = nullptr;
    QToolButton *m_clearHistoryButton = nullptr;
    QFrame *m_globalStatusBanner = nullptr;
    QLabel *m_globalStatusLabel = nullptr;
    bool m_confirmCancelAll = true;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_tasksContainer = nullptr;
    QVBoxLayout *m_tasksLayout = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QHash<quint64, TransferQueueTaskItemWidget *> m_itemWidgets;
    QList<QPointer<TransferQueueTaskItemWidget>> m_retiredItemWidgets;
};
