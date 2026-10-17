/*
 * Implementation of dialog for advanced search filter parameters (Stage 3B.1).
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "advancedsearchfiltersdialog.h"
#include "browsercommon.h"

#include <QDialogButtonBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>

AdvancedSearchFiltersDialog::AdvancedSearchFiltersDialog(const SearchQuery &currentQuery, QWidget *parent)
    : QDialog(parent)
    , m_baseQuery(currentQuery)
{
    setupUi();
}

void AdvancedSearchFiltersDialog::setupUi()
{
    setWindowTitle(trLocal("Zaawansowane filtry wyszukiwania", "Advanced Search Filters"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowModality(Qt::NonModal);
    setMinimumWidth(440);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(14);

    auto *formLayout = new QFormLayout();
    formLayout->setSpacing(12);
    formLayout->setLabelAlignment(Qt::AlignLeft);

    // 1. Name matching mode
    m_nameModeCombo = new QComboBox(this);
    m_nameModeCombo->addItem(
        trLocal("Automatycznie (tekst lub maska)", "Auto (text or glob)"),
        static_cast<int>(SearchQuery::NameMode::Auto));
    m_nameModeCombo->addItem(
        trLocal("Zawiera tekst (podciąg)", "Contains text (substring)"),
        static_cast<int>(SearchQuery::NameMode::Substring));
    m_nameModeCombo->addItem(
        trLocal("Maska nazwy / glob (*, ?)", "Filename pattern / glob (*, ?)"),
        static_cast<int>(SearchQuery::NameMode::Glob));

    const int modeIndex = m_nameModeCombo->findData(static_cast<int>(m_baseQuery.nameMode()));
    if (modeIndex >= 0) {
        m_nameModeCombo->setCurrentIndex(modeIndex);
    }
    m_nameModeCombo->setToolTip(trLocal(
        "Wybierz tryb dopasowywania nazwy:\n"
        "• Auto: automatycznie używa maski glob, gdy zapytanie zawiera * lub ?, w przeciwnym razie dopasowuje tekst.\n"
        "• Podciąg: wyszukuje wpisany tekst w dowolnej części nazwy.\n"
        "• Maska / glob: umożliwia użycie znaków wieloznacznych (* dla dowolnych znaków, ? dla pojedynczego znaku).",
        "Select name matching mode:\n"
        "• Auto: automatically uses glob pattern when query contains * or ?, otherwise matches substring.\n"
        "• Substring: searches for text contained anywhere within the filename.\n"
        "• Pattern / glob: allows wildcards (* for any characters, ? for a single character)."));
    formLayout->addRow(trLocal("Dopasowanie nazwy:", "Name matching:"), m_nameModeCombo);

    // 2. Case sensitivity
    m_caseSensitiveCheck = new QCheckBox(
        trLocal("Rozróżniaj wielkość liter (dotyczy nazwy)", "Match case (applies to filename)"),
        this);
    m_caseSensitiveCheck->setChecked(m_baseQuery.matchCase());
    m_caseSensitiveCheck->setToolTip(trLocal(
        "Uwzględniaj wielkość liter przy dopasowywaniu nazwy pliku lub folderu.\n"
        "Filtr rozszerzeń pozostaje zawsze niewrażliwy na wielkość liter.",
        "Match case when searching file or folder names.\n"
        "Extension filtering remains case-insensitive."));
    formLayout->addRow(QString(), m_caseSensitiveCheck);

    // 3. Extensions
    m_extensionsEdit = new QLineEdit(this);
    m_extensionsEdit->setClearButtonEnabled(true);
    m_extensionsEdit->setPlaceholderText(
        trLocal("np. jpg, png, tar.gz", "e.g. jpg, png, tar.gz"));
    if (!m_baseQuery.extensions().isEmpty()) {
        m_extensionsEdit->setText(m_baseQuery.extensions().join(QStringLiteral(", ")));
    }
    m_extensionsEdit->setToolTip(trLocal(
        "Rozszerzenia rozdzielone przecinkami (np. txt, pdf, tar.gz).\n"
        "Wielkość liter jest ignorowana. Kropki wiodące są usuwane automatycznie.",
        "Comma-separated extensions (e.g. txt, pdf, tar.gz).\n"
        "Case-insensitive. Leading dots are stripped automatically."));
    formLayout->addRow(trLocal("Rozszerzenia:", "Extensions:"), m_extensionsEdit);

    // 4. Target type (Files / Folders / All)
    m_targetTypeCombo = new QComboBox(this);
    m_targetTypeCombo->addItem(
        trLocal("Wszystkie (pliki i foldery)", "All items (files and folders)"),
        static_cast<int>(SearchQuery::TargetType::All));
    m_targetTypeCombo->addItem(
        trLocal("Tylko pliki", "Files only"),
        static_cast<int>(SearchQuery::TargetType::FilesOnly));
    m_targetTypeCombo->addItem(
        trLocal("Tylko foldery", "Folders only"),
        static_cast<int>(SearchQuery::TargetType::FoldersOnly));

    const int targetIndex = m_targetTypeCombo->findData(static_cast<int>(m_baseQuery.targetType()));
    if (targetIndex >= 0) {
        m_targetTypeCombo->setCurrentIndex(targetIndex);
    }
    m_targetTypeCombo->setToolTip(trLocal(
        "Ogranicz wyniki do samych plików, samych folderów lub uwzględniaj oba typy.",
        "Restrict results to files only, folders only, or include all items."));
    formLayout->addRow(trLocal("Typ elementów:", "Target type:"), m_targetTypeCombo);

    // 5. File categories (multi-select check boxes)
    auto *catContainer = new QWidget(this);
    auto *catLayout = new QGridLayout(catContainer);
    catLayout->setContentsMargins(0, 0, 0, 0);
    catLayout->setHorizontalSpacing(16);
    catLayout->setVerticalSpacing(6);

    m_catImagesCheck = new QCheckBox(trLocal("Obrazy", "Images"), catContainer);
    m_catDocsCheck = new QCheckBox(trLocal("Dokumenty", "Documents"), catContainer);
    m_catAudioCheck = new QCheckBox(trLocal("Muzyka / audio", "Music / audio"), catContainer);
    m_catVideoCheck = new QCheckBox(trLocal("Wideo", "Video"), catContainer);
    m_catArchivesCheck = new QCheckBox(trLocal("Archiwa", "Archives"), catContainer);

    catLayout->addWidget(m_catImagesCheck, 0, 0);
    catLayout->addWidget(m_catDocsCheck, 0, 1);
    catLayout->addWidget(m_catAudioCheck, 1, 0);
    catLayout->addWidget(m_catVideoCheck, 1, 1);
    catLayout->addWidget(m_catArchivesCheck, 2, 0);

    const auto &initialCats = m_baseQuery.categories();
    m_catImagesCheck->setChecked(initialCats.contains(SearchQuery::Category::Images));
    m_catDocsCheck->setChecked(initialCats.contains(SearchQuery::Category::Documents));
    m_catAudioCheck->setChecked(initialCats.contains(SearchQuery::Category::Audio));
    m_catVideoCheck->setChecked(initialCats.contains(SearchQuery::Category::Video));
    m_catArchivesCheck->setChecked(initialCats.contains(SearchQuery::Category::Archives));

    catContainer->setToolTip(trLocal(
        "Wybierz jedną lub więcej kategorii (logika LUB wewnątrz grupy).\n"
        "Pozostawienie wszystkich pól odznaczonych oznacza brak ograniczenia kategorii.",
        "Select one or more categories (OR logic within the group).\n"
        "Leaving all unchecked means no category restriction."));
    formLayout->addRow(trLocal("Kategorie plików:", "File categories:"), catContainer);

    // 6. MIME types
    m_mimesEdit = new QLineEdit(this);
    m_mimesEdit->setClearButtonEnabled(true);
    m_mimesEdit->setPlaceholderText(
        trLocal("np. image/png, application/pdf, text/*", "e.g. image/png, application/pdf, text/*"));
    if (!m_baseQuery.mimeTypes().isEmpty()) {
        m_mimesEdit->setText(m_baseQuery.mimeTypes().join(QStringLiteral(", ")));
    }
    m_mimesEdit->setToolTip(trLocal(
        "Wprowadź identyfikatory lub wzorce MIME rozdzielone przecinkami (np. image/png, application/pdf, text/*).\n"
        "Wiele typów łączy się logiką LUB. Puste pole oznacza brak ograniczenia.",
        "Enter comma-separated MIME types or wildcard patterns (e.g. image/png, application/pdf, text/*).\n"
        "Multiple types combine with OR. Empty means no restriction."));
    formLayout->addRow(trLocal("Typy MIME:", "MIME types:"), m_mimesEdit);

    // 7. Modification date range
    auto *dateContainer = new QWidget(this);
    auto *dateLayout = new QGridLayout(dateContainer);
    dateLayout->setContentsMargins(0, 0, 0, 0);
    dateLayout->setHorizontalSpacing(8);
    dateLayout->setVerticalSpacing(6);

    m_dateMinCheck = new QCheckBox(trLocal("Od daty:", "From date:"), dateContainer);
    m_dateMinEdit = new QDateEdit(dateContainer);
    m_dateMinEdit->setCalendarPopup(true);
    m_dateMinEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateMinEdit->setDate(QDate::currentDate());
    m_dateMinEdit->setEnabled(false);

    m_dateMaxCheck = new QCheckBox(trLocal("Do daty:", "To date:"), dateContainer);
    m_dateMaxEdit = new QDateEdit(dateContainer);
    m_dateMaxEdit->setCalendarPopup(true);
    m_dateMaxEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateMaxEdit->setDate(QDate::currentDate());
    m_dateMaxEdit->setEnabled(false);

    dateLayout->addWidget(m_dateMinCheck, 0, 0);
    dateLayout->addWidget(m_dateMinEdit, 0, 1);
    dateLayout->addWidget(m_dateMaxCheck, 1, 0);
    dateLayout->addWidget(m_dateMaxEdit, 1, 1);

    connect(m_dateMinCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_dateMinEdit->setEnabled(checked);
        validateInputs();
    });
    connect(m_dateMaxCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_dateMaxEdit->setEnabled(checked);
        validateInputs();
    });
    connect(m_dateMinEdit, &QDateEdit::dateChanged, this, [this] {
        validateInputs();
    });
    connect(m_dateMaxEdit, &QDateEdit::dateChanged, this, [this] {
        validateInputs();
    });

    dateContainer->setToolTip(trLocal(
        "Filtruj wg daty modyfikacji (od początku wybranego dnia do końca wybranego dnia).\n"
        "Oba warunki są opcjonalne.",
        "Filter by modification date (inclusive bounds).\n"
        "Both boundaries are optional."));
    formLayout->addRow(trLocal("Data modyfikacji:", "Modification date:"), dateContainer);

    // 8. File size range
    const QStringList sizeUnits = {
        QStringLiteral("B"),
        QStringLiteral("KiB"),
        QStringLiteral("MiB"),
        QStringLiteral("GiB"),
        QStringLiteral("TiB")
    };

    auto *sizeContainer = new QWidget(this);
    auto *sizeLayout = new QGridLayout(sizeContainer);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setHorizontalSpacing(8);
    sizeLayout->setVerticalSpacing(6);

    m_sizePresetCombo = new QComboBox(sizeContainer);
    m_sizePresetCombo->addItem(trLocal("Dowolny / niestandardowy", "Any / custom"), 0);
    m_sizePresetCombo->addItem(trLocal("Mniejszy niż 1 MiB", "Smaller than 1 MiB"), 1);
    m_sizePresetCombo->addItem(trLocal("1–100 MiB", "1–100 MiB"), 2);
    m_sizePresetCombo->addItem(trLocal("100 MiB–1 GiB", "100 MiB–1 GiB"), 3);
    m_sizePresetCombo->addItem(trLocal("Większy niż 1 GiB", "Larger than 1 GiB"), 4);

    m_sizeMinCheck = new QCheckBox(trLocal("Od:", "From:"), sizeContainer);
    m_sizeMinSpin = new QDoubleSpinBox(sizeContainer);
    m_sizeMinSpin->setRange(0.0, 999999999.0);
    m_sizeMinSpin->setDecimals(2);
    m_sizeMinSpin->setValue(0.0);
    m_sizeMinSpin->setEnabled(false);

    m_sizeMinUnitCombo = new QComboBox(sizeContainer);
    m_sizeMinUnitCombo->addItems(sizeUnits);
    m_sizeMinUnitCombo->setCurrentIndex(2); // Default MiB
    m_sizeMinUnitCombo->setEnabled(false);

    m_sizeMaxCheck = new QCheckBox(trLocal("Do:", "To:"), sizeContainer);
    m_sizeMaxSpin = new QDoubleSpinBox(sizeContainer);
    m_sizeMaxSpin->setRange(0.0, 999999999.0);
    m_sizeMaxSpin->setDecimals(2);
    m_sizeMaxSpin->setValue(0.0);
    m_sizeMaxSpin->setEnabled(false);

    m_sizeMaxUnitCombo = new QComboBox(sizeContainer);
    m_sizeMaxUnitCombo->addItems(sizeUnits);
    m_sizeMaxUnitCombo->setCurrentIndex(2); // Default MiB
    m_sizeMaxUnitCombo->setEnabled(false);

    sizeLayout->addWidget(new QLabel(trLocal("Szybki wybór:", "Quick preset:"), sizeContainer), 0, 0);
    sizeLayout->addWidget(m_sizePresetCombo, 0, 1, 1, 2);
    sizeLayout->addWidget(m_sizeMinCheck, 1, 0);
    sizeLayout->addWidget(m_sizeMinSpin, 1, 1);
    sizeLayout->addWidget(m_sizeMinUnitCombo, 1, 2);
    sizeLayout->addWidget(m_sizeMaxCheck, 2, 0);
    sizeLayout->addWidget(m_sizeMaxSpin, 2, 1);
    sizeLayout->addWidget(m_sizeMaxUnitCombo, 2, 2);

    connect(m_sizePresetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index > 0) {
            // Selecting a preset unchecks custom bounds
            const QSignalBlocker b1(m_sizeMinCheck);
            const QSignalBlocker b2(m_sizeMaxCheck);
            m_sizeMinCheck->setChecked(false);
            m_sizeMinSpin->setEnabled(false);
            m_sizeMinUnitCombo->setEnabled(false);
            m_sizeMaxCheck->setChecked(false);
            m_sizeMaxSpin->setEnabled(false);
            m_sizeMaxUnitCombo->setEnabled(false);
            validateInputs();
        }
    });

    const auto onCustomSizeChanged = [this] {
        if ((m_sizeMinCheck && m_sizeMinCheck->isChecked()) || (m_sizeMaxCheck && m_sizeMaxCheck->isChecked())) {
            if (m_sizePresetCombo && m_sizePresetCombo->currentIndex() != 0) {
                const QSignalBlocker b(m_sizePresetCombo);
                m_sizePresetCombo->setCurrentIndex(0);
            }
        }
        validateInputs();
    };

    connect(m_sizeMinCheck, &QCheckBox::toggled, this, [this, onCustomSizeChanged](bool checked) {
        m_sizeMinSpin->setEnabled(checked);
        m_sizeMinUnitCombo->setEnabled(checked);
        onCustomSizeChanged();
    });
    connect(m_sizeMaxCheck, &QCheckBox::toggled, this, [this, onCustomSizeChanged](bool checked) {
        m_sizeMaxSpin->setEnabled(checked);
        m_sizeMaxUnitCombo->setEnabled(checked);
        onCustomSizeChanged();
    });
    connect(m_sizeMinSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, onCustomSizeChanged);
    connect(m_sizeMaxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, onCustomSizeChanged);
    connect(m_sizeMinUnitCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, onCustomSizeChanged);
    connect(m_sizeMaxUnitCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, onCustomSizeChanged);

    sizeContainer->setToolTip(trLocal(
        "Filtruj wg rozmiaru pliku przy użyciu szybkiego presetu lub niestandardowego zakresu (jednostki IEC: B, KiB, MiB, GiB, TiB).\n"
        "Filtrowanie rozmiaru dotyczy wyłącznie plików (foldery są wykluczone).",
        "Filter by file size using a quick preset or custom range (IEC units: B, KiB, MiB, GiB, TiB).\n"
        "Size filtering applies to files only (folders are excluded)."));
    formLayout->addRow(trLocal("Rozmiar pliku:", "File size:"), sizeContainer);

    rootLayout->addLayout(formLayout);

    // Validation error message label
    m_validationErrorLabel = new QLabel(this);
    m_validationErrorLabel->setStyleSheet(QStringLiteral("color: #e05252; font-weight: bold;"));
    m_validationErrorLabel->setWordWrap(true);
    m_validationErrorLabel->hide();
    rootLayout->addWidget(m_validationErrorLabel);

    // Buttons
    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_applyButton = m_buttonBox->button(QDialogButtonBox::Ok);
    if (m_applyButton) {
        m_applyButton->setText(trLocal("Zastosuj", "Apply"));
        m_applyButton->setDefault(true);
    }
    QPushButton *cancelBtn = m_buttonBox->button(QDialogButtonBox::Cancel);
    if (cancelBtn) {
        cancelBtn->setText(trLocal("Anuluj", "Cancel"));
    }

    m_clearButton = new QPushButton(trLocal("Wyczyść", "Clear"), this);
    m_clearButton->setToolTip(trLocal(
        "Przywróć domyślne ustawienia zaawansowanych filtrów",
        "Reset advanced filters to defaults"));
    m_buttonBox->addButton(m_clearButton, QDialogButtonBox::ResetRole);

    setAccessibleName(windowTitle());
    m_nameModeCombo->setAccessibleName(trLocal("Dopasowanie nazwy", "Name matching"));
    m_nameModeCombo->setAccessibleDescription(m_nameModeCombo->toolTip());
    m_caseSensitiveCheck->setAccessibleName(m_caseSensitiveCheck->text());
    m_caseSensitiveCheck->setAccessibleDescription(m_caseSensitiveCheck->toolTip());
    m_extensionsEdit->setAccessibleName(trLocal("Rozszerzenia", "Extensions"));
    m_extensionsEdit->setAccessibleDescription(m_extensionsEdit->toolTip());
    m_targetTypeCombo->setAccessibleName(trLocal("Typ elementów", "Target type"));
    m_targetTypeCombo->setAccessibleDescription(m_targetTypeCombo->toolTip());
    m_catImagesCheck->setAccessibleName(m_catImagesCheck->text());
    m_catDocsCheck->setAccessibleName(m_catDocsCheck->text());
    m_catAudioCheck->setAccessibleName(m_catAudioCheck->text());
    m_catVideoCheck->setAccessibleName(m_catVideoCheck->text());
    m_catArchivesCheck->setAccessibleName(m_catArchivesCheck->text());
    m_mimesEdit->setAccessibleName(trLocal("Typy MIME", "MIME types"));
    m_mimesEdit->setAccessibleDescription(m_mimesEdit->toolTip());
    m_dateMinCheck->setAccessibleName(m_dateMinCheck->text());
    m_dateMinEdit->setAccessibleName(trLocal("Data początkowa modyfikacji", "Modification start date"));
    m_dateMaxCheck->setAccessibleName(m_dateMaxCheck->text());
    m_dateMaxEdit->setAccessibleName(trLocal("Data końcowa modyfikacji", "Modification end date"));
    m_sizePresetCombo->setAccessibleName(trLocal("Szybki wybór rozmiaru pliku", "File size quick preset"));
    m_sizeMinCheck->setAccessibleName(trLocal("Minimalny rozmiar pliku", "Minimum file size"));
    m_sizeMinSpin->setAccessibleName(trLocal("Wartość minimalnego rozmiaru pliku", "Minimum file size value"));
    m_sizeMinUnitCombo->setAccessibleName(trLocal("Jednostka minimalnego rozmiaru pliku", "Minimum file size unit"));
    m_sizeMaxCheck->setAccessibleName(trLocal("Maksymalny rozmiar pliku", "Maximum file size"));
    m_sizeMaxSpin->setAccessibleName(trLocal("Wartość maksymalnego rozmiaru pliku", "Maximum file size value"));
    m_sizeMaxUnitCombo->setAccessibleName(trLocal("Jednostka maksymalnego rozmiaru pliku", "Maximum file size unit"));
    m_validationErrorLabel->setAccessibleName(trLocal("Komunikat walidacji filtrów", "Filter validation message"));
    if (m_applyButton) {
        m_applyButton->setAccessibleName(m_applyButton->text());
    }
    if (cancelBtn) {
        cancelBtn->setAccessibleName(cancelBtn->text());
    }
    if (m_clearButton) {
        m_clearButton->setAccessibleName(m_clearButton->text());
        m_clearButton->setAccessibleDescription(m_clearButton->toolTip());
    }

    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &AdvancedSearchFiltersDialog::onApply);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_clearButton, &QPushButton::clicked, this, &AdvancedSearchFiltersDialog::resetToDefaults);

    rootLayout->addWidget(m_buttonBox);

    // Populate initial date and size controls
    setQuery(m_baseQuery);
}

quint64 AdvancedSearchFiltersDialog::unitMultiplier(int index)
{
    switch (index) {
    case 0: return 1ULL;
    case 1: return 1024ULL;
    case 2: return 1024ULL * 1024ULL;
    case 3: return 1024ULL * 1024ULL * 1024ULL;
    case 4: return 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    default: return 1ULL;
    }
}

void AdvancedSearchFiltersDialog::onApply()
{
    validateInputs();
    if (m_applyButton && !m_applyButton->isEnabled()) {
        return;
    }
    Q_EMIT filtersApplied(configuredQuery());
    accept();
}

void AdvancedSearchFiltersDialog::validateInputs()
{
    QString error;

    if (m_dateMinCheck && m_dateMaxCheck && m_dateMinCheck->isChecked() && m_dateMaxCheck->isChecked()) {
        if (m_dateMinEdit->date() > m_dateMaxEdit->date()) {
            error = trLocal("Data początkowa nie może być późniejsza niż data końcowa.",
                            "Start date cannot be later than end date.");
        }
    }

    if (error.isEmpty() && m_sizeMinCheck && m_sizeMaxCheck && m_sizeMinCheck->isChecked() && m_sizeMaxCheck->isChecked()) {
        const double minVal = m_sizeMinSpin->value();
        const double maxVal = m_sizeMaxSpin->value();
        const quint64 minMul = unitMultiplier(m_sizeMinUnitCombo->currentIndex());
        const quint64 maxMul = unitMultiplier(m_sizeMaxUnitCombo->currentIndex());

        const double minBytes = minVal * static_cast<double>(minMul);
        const double maxBytes = maxVal * static_cast<double>(maxMul);

        if (minBytes > maxBytes) {
            error = trLocal("Rozmiar minimalny nie może być większy niż maksymalny.",
                            "Minimum size cannot be greater than maximum size.");
        }
    }

    if (!error.isEmpty()) {
        m_validationErrorLabel->setText(error);
        m_validationErrorLabel->show();
        if (m_applyButton) {
            m_applyButton->setEnabled(false);
        }
    } else {
        m_validationErrorLabel->clear();
        m_validationErrorLabel->hide();
        if (m_applyButton) {
            m_applyButton->setEnabled(true);
        }
    }
}

static void populateSizeControl(qint64 bytes, QDoubleSpinBox *spin, QComboBox *combo)
{
    if (bytes <= 0) {
        spin->setValue(0.0);
        combo->setCurrentIndex(0);
        return;
    }
    const quint64 b = static_cast<quint64>(bytes);
    const quint64 tib = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    const quint64 gib = 1024ULL * 1024ULL * 1024ULL;
    const quint64 mib = 1024ULL * 1024ULL;
    const quint64 kib = 1024ULL;

    if (b >= tib && (b % tib == 0 || b >= 10 * tib)) {
        spin->setValue(static_cast<double>(b) / static_cast<double>(tib));
        combo->setCurrentIndex(4);
    } else if (b >= gib && (b % gib == 0 || b >= 10 * gib)) {
        spin->setValue(static_cast<double>(b) / static_cast<double>(gib));
        combo->setCurrentIndex(3);
    } else if (b >= mib && (b % mib == 0 || b >= 10 * mib)) {
        spin->setValue(static_cast<double>(b) / static_cast<double>(mib));
        combo->setCurrentIndex(2);
    } else if (b >= kib && (b % kib == 0 || b >= 10 * kib)) {
        spin->setValue(static_cast<double>(b) / static_cast<double>(kib));
        combo->setCurrentIndex(1);
    } else {
        spin->setValue(static_cast<double>(b));
        combo->setCurrentIndex(0);
    }
}

void AdvancedSearchFiltersDialog::setQuery(const SearchQuery &query)
{
    m_baseQuery = query;

    const int modeIndex = m_nameModeCombo->findData(static_cast<int>(query.nameMode()));
    if (modeIndex >= 0) {
        m_nameModeCombo->setCurrentIndex(modeIndex);
    }
    m_caseSensitiveCheck->setChecked(query.matchCase());

    if (!query.extensions().isEmpty()) {
        m_extensionsEdit->setText(query.extensions().join(QStringLiteral(", ")));
    } else {
        m_extensionsEdit->clear();
    }

    const int targetIndex = m_targetTypeCombo->findData(static_cast<int>(query.targetType()));
    if (targetIndex >= 0) {
        m_targetTypeCombo->setCurrentIndex(targetIndex);
    }

    const auto &cats = query.categories();
    m_catImagesCheck->setChecked(cats.contains(SearchQuery::Category::Images));
    m_catDocsCheck->setChecked(cats.contains(SearchQuery::Category::Documents));
    m_catAudioCheck->setChecked(cats.contains(SearchQuery::Category::Audio));
    m_catVideoCheck->setChecked(cats.contains(SearchQuery::Category::Video));
    m_catArchivesCheck->setChecked(cats.contains(SearchQuery::Category::Archives));

    if (!query.mimeTypes().isEmpty()) {
        m_mimesEdit->setText(query.mimeTypes().join(QStringLiteral(", ")));
    } else {
        m_mimesEdit->clear();
    }

    // Dates
    const QTimeZone sysZone = QTimeZone::systemTimeZone();
    if (query.dateMin().has_value()) {
        m_dateMinCheck->setChecked(true);
        m_dateMinEdit->setEnabled(true);
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(*query.dateMin(), sysZone);
        m_dateMinEdit->setDate(dt.date());
    } else {
        m_dateMinCheck->setChecked(false);
        m_dateMinEdit->setEnabled(false);
        m_dateMinEdit->setDate(QDate::currentDate());
    }

    if (query.dateMax().has_value()) {
        m_dateMaxCheck->setChecked(true);
        m_dateMaxEdit->setEnabled(true);
        const QDateTime dt = QDateTime::fromSecsSinceEpoch(*query.dateMax(), sysZone);
        m_dateMaxEdit->setDate(dt.date());
    } else {
        m_dateMaxCheck->setChecked(false);
        m_dateMaxEdit->setEnabled(false);
        m_dateMaxEdit->setDate(QDate::currentDate());
    }

    // Sizes
    {
        const QSignalBlocker bMinCheck(m_sizeMinCheck);
        const QSignalBlocker bMaxCheck(m_sizeMaxCheck);
        const QSignalBlocker bMinSpin(m_sizeMinSpin);
        const QSignalBlocker bMaxSpin(m_sizeMaxSpin);
        const QSignalBlocker bMinUnit(m_sizeMinUnitCombo);
        const QSignalBlocker bMaxUnit(m_sizeMaxUnitCombo);

        if (query.sizeMin().has_value() || query.sizeMax().has_value()) {
            if (m_sizePresetCombo) {
                const QSignalBlocker b(m_sizePresetCombo);
                m_sizePresetCombo->setCurrentIndex(0);
            }
            if (query.sizeMin().has_value()) {
                m_sizeMinCheck->setChecked(true);
                m_sizeMinSpin->setEnabled(true);
                m_sizeMinUnitCombo->setEnabled(true);
                populateSizeControl(*query.sizeMin(), m_sizeMinSpin, m_sizeMinUnitCombo);
            } else {
                m_sizeMinCheck->setChecked(false);
                m_sizeMinSpin->setEnabled(false);
                m_sizeMinUnitCombo->setEnabled(false);
                m_sizeMinSpin->setValue(0.0);
                m_sizeMinUnitCombo->setCurrentIndex(2); // MiB
            }

            if (query.sizeMax().has_value()) {
                m_sizeMaxCheck->setChecked(true);
                m_sizeMaxSpin->setEnabled(true);
                m_sizeMaxUnitCombo->setEnabled(true);
                populateSizeControl(*query.sizeMax(), m_sizeMaxSpin, m_sizeMaxUnitCombo);
            } else {
                m_sizeMaxCheck->setChecked(false);
                m_sizeMaxSpin->setEnabled(false);
                m_sizeMaxUnitCombo->setEnabled(false);
                m_sizeMaxSpin->setValue(0.0);
                m_sizeMaxUnitCombo->setCurrentIndex(2); // MiB
            }
        } else {
            // No custom size range: check if a legacy size preset is active
            const int preset = std::clamp(query.legacySize(), 0, 4);
            if (m_sizePresetCombo) {
                const QSignalBlocker b(m_sizePresetCombo);
                m_sizePresetCombo->setCurrentIndex(preset);
            }
            m_sizeMinCheck->setChecked(false);
            m_sizeMinSpin->setEnabled(false);
            m_sizeMinUnitCombo->setEnabled(false);
            m_sizeMinSpin->setValue(0.0);
            m_sizeMinUnitCombo->setCurrentIndex(2); // MiB

            m_sizeMaxCheck->setChecked(false);
            m_sizeMaxSpin->setEnabled(false);
            m_sizeMaxUnitCombo->setEnabled(false);
            m_sizeMaxSpin->setValue(0.0);
            m_sizeMaxUnitCombo->setCurrentIndex(2); // MiB
        }
    }

    validateInputs();
}

void AdvancedSearchFiltersDialog::resetToDefaults()
{
    const int autoIndex = m_nameModeCombo->findData(static_cast<int>(SearchQuery::NameMode::Auto));
    if (autoIndex >= 0) {
        m_nameModeCombo->setCurrentIndex(autoIndex);
    }
    m_caseSensitiveCheck->setChecked(false);
    m_extensionsEdit->clear();
    const int allIndex = m_targetTypeCombo->findData(static_cast<int>(SearchQuery::TargetType::All));
    if (allIndex >= 0) {
        m_targetTypeCombo->setCurrentIndex(allIndex);
    }
    m_catImagesCheck->setChecked(false);
    m_catDocsCheck->setChecked(false);
    m_catAudioCheck->setChecked(false);
    m_catVideoCheck->setChecked(false);
    m_catArchivesCheck->setChecked(false);
    m_mimesEdit->clear();

    m_dateMinCheck->setChecked(false);
    m_dateMinEdit->setEnabled(false);
    m_dateMinEdit->setDate(QDate::currentDate());
    m_dateMaxCheck->setChecked(false);
    m_dateMaxEdit->setEnabled(false);
    m_dateMaxEdit->setDate(QDate::currentDate());

    if (m_sizePresetCombo) {
        const QSignalBlocker b(m_sizePresetCombo);
        m_sizePresetCombo->setCurrentIndex(0);
    }

    {
        const QSignalBlocker bMinCheck(m_sizeMinCheck);
        const QSignalBlocker bMaxCheck(m_sizeMaxCheck);
        const QSignalBlocker bMinSpin(m_sizeMinSpin);
        const QSignalBlocker bMaxSpin(m_sizeMaxSpin);
        const QSignalBlocker bMinUnit(m_sizeMinUnitCombo);
        const QSignalBlocker bMaxUnit(m_sizeMaxUnitCombo);

        m_sizeMinCheck->setChecked(false);
        m_sizeMinSpin->setEnabled(false);
        m_sizeMinUnitCombo->setEnabled(false);
        m_sizeMinSpin->setValue(0.0);
        m_sizeMinUnitCombo->setCurrentIndex(2); // MiB

        m_sizeMaxCheck->setChecked(false);
        m_sizeMaxSpin->setEnabled(false);
        m_sizeMaxUnitCombo->setEnabled(false);
        m_sizeMaxSpin->setValue(0.0);
        m_sizeMaxUnitCombo->setCurrentIndex(2); // MiB
    }

    validateInputs();
}

SearchQuery::NameMode AdvancedSearchFiltersDialog::nameMode() const
{
    return static_cast<SearchQuery::NameMode>(
        m_nameModeCombo->currentData().toInt());
}

bool AdvancedSearchFiltersDialog::matchCase() const
{
    return m_caseSensitiveCheck->isChecked();
}

QStringList AdvancedSearchFiltersDialog::extensions() const
{
    const QString text = m_extensionsEdit->text().trimmed();
    if (text.isEmpty()) {
        return {};
    }
    const QStringList rawList = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList result;
    for (const QString &raw : rawList) {
        const QString normalized = SearchQuery::normalizeExtension(raw);
        if (!normalized.isEmpty() && !result.contains(normalized)) {
            result.append(normalized);
        }
    }
    return result;
}

SearchQuery::TargetType AdvancedSearchFiltersDialog::targetType() const
{
    return static_cast<SearchQuery::TargetType>(
        m_targetTypeCombo->currentData().toInt());
}

QList<SearchQuery::Category> AdvancedSearchFiltersDialog::categories() const
{
    QList<SearchQuery::Category> result;
    if (m_catImagesCheck->isChecked()) {
        result.append(SearchQuery::Category::Images);
    }
    if (m_catDocsCheck->isChecked()) {
        result.append(SearchQuery::Category::Documents);
    }
    if (m_catAudioCheck->isChecked()) {
        result.append(SearchQuery::Category::Audio);
    }
    if (m_catVideoCheck->isChecked()) {
        result.append(SearchQuery::Category::Video);
    }
    if (m_catArchivesCheck->isChecked()) {
        result.append(SearchQuery::Category::Archives);
    }
    return result;
}

QStringList AdvancedSearchFiltersDialog::mimeTypes() const
{
    const QString text = m_mimesEdit->text().trimmed();
    if (text.isEmpty()) {
        return {};
    }
    const QStringList rawList = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    QStringList result;
    for (const QString &raw : rawList) {
        const QString trimmed = raw.trimmed().toLower();
        if (!trimmed.isEmpty() && !result.contains(trimmed)) {
            result.append(trimmed);
        }
    }
    return result;
}

std::optional<qint64> AdvancedSearchFiltersDialog::dateMin() const
{
    if (!m_dateMinCheck || !m_dateMinCheck->isChecked()) {
        return std::nullopt;
    }
    const QDateTime start = m_dateMinEdit->date().startOfDay(QTimeZone::systemTimeZone());
    return start.isValid() ? std::optional<qint64>(start.toSecsSinceEpoch()) : std::nullopt;
}

std::optional<qint64> AdvancedSearchFiltersDialog::dateMax() const
{
    if (!m_dateMaxCheck || !m_dateMaxCheck->isChecked()) {
        return std::nullopt;
    }
    const QDateTime next = m_dateMaxEdit->date().addDays(1).startOfDay(QTimeZone::systemTimeZone());
    return next.isValid() ? std::optional<qint64>(next.toSecsSinceEpoch() - 1) : std::nullopt;
}

std::optional<qint64> AdvancedSearchFiltersDialog::sizeMin() const
{
    if (!m_sizeMinCheck || !m_sizeMinCheck->isChecked()) {
        return std::nullopt;
    }
    const double val = m_sizeMinSpin->value();
    const quint64 mul = unitMultiplier(m_sizeMinUnitCombo->currentIndex());
    const double bytesD = val * static_cast<double>(mul);
    if (bytesD < 0.0) return 0;
    if (bytesD >= static_cast<double>(INT64_MAX)) return INT64_MAX;
    return static_cast<qint64>(bytesD);
}

std::optional<qint64> AdvancedSearchFiltersDialog::sizeMax() const
{
    if (!m_sizeMaxCheck || !m_sizeMaxCheck->isChecked()) {
        return std::nullopt;
    }
    const double val = m_sizeMaxSpin->value();
    const quint64 mul = unitMultiplier(m_sizeMaxUnitCombo->currentIndex());
    const double bytesD = val * static_cast<double>(mul);
    if (bytesD < 0.0) return 0;
    if (bytesD >= static_cast<double>(INT64_MAX)) return INT64_MAX;
    return static_cast<qint64>(bytesD);
}

int AdvancedSearchFiltersDialog::legacySize() const
{
    if (sizeMin().has_value() || sizeMax().has_value()) {
        return 0;
    }
    if (m_sizePresetCombo) {
        return m_sizePresetCombo->currentIndex();
    }
    return 0;
}

SearchQuery AdvancedSearchFiltersDialog::configuredQuery() const
{
    SearchQuery query = m_baseQuery;
    query.setNameMode(nameMode());
    query.setMatchCase(matchCase());
    query.setExtensions(extensions());
    query.setTargetType(targetType());
    query.setCategories(categories());
    query.setMimeTypes(mimeTypes());
    query.setDateRange(dateMin(), dateMax());
    query.setSizeRange(sizeMin(), sizeMax());
    query.setLegacySize(legacySize());
    return query;
}
