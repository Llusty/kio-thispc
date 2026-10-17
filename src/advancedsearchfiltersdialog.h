/*
 * Dialog for advanced search filter parameters (Stage 3B.1).
 *
 * Provides controls for:
 *   - Name matching mode (substring vs glob / wildcard)
 *   - Case sensitivity
 *   - Comma-separated extension list
 *   - Target item type (all items, files only, folders only)
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "searchquery.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;

class AdvancedSearchFiltersDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdvancedSearchFiltersDialog(const SearchQuery &currentQuery, QWidget *parent = nullptr);
    ~AdvancedSearchFiltersDialog() override = default;

    SearchQuery configuredQuery() const;

    SearchQuery::NameMode nameMode() const;
    bool matchCase() const;
    QStringList extensions() const;
    SearchQuery::TargetType targetType() const;
    QList<SearchQuery::Category> categories() const;
    QStringList mimeTypes() const;
    std::optional<qint64> dateMin() const;
    std::optional<qint64> dateMax() const;
    std::optional<qint64> sizeMin() const;
    std::optional<qint64> sizeMax() const;
    int legacySize() const;

    void setQuery(const SearchQuery &query);

Q_SIGNALS:
    void filtersApplied(const SearchQuery &query);

public Q_SLOTS:
    void resetToDefaults();
    void onApply();
    void validateInputs();

private:
    void setupUi();
    static quint64 unitMultiplier(int index);

    SearchQuery m_baseQuery;

    QComboBox *m_nameModeCombo = nullptr;
    QCheckBox *m_caseSensitiveCheck = nullptr;
    QLineEdit *m_extensionsEdit = nullptr;
    QComboBox *m_targetTypeCombo = nullptr;

    // Categories
    QCheckBox *m_catImagesCheck = nullptr;
    QCheckBox *m_catDocsCheck = nullptr;
    QCheckBox *m_catAudioCheck = nullptr;
    QCheckBox *m_catVideoCheck = nullptr;
    QCheckBox *m_catArchivesCheck = nullptr;

    // MIME
    QLineEdit *m_mimesEdit = nullptr;

    // Date modification range
    QCheckBox *m_dateMinCheck = nullptr;
    QDateEdit *m_dateMinEdit = nullptr;
    QCheckBox *m_dateMaxCheck = nullptr;
    QDateEdit *m_dateMaxEdit = nullptr;

    // File size range
    QComboBox *m_sizePresetCombo = nullptr;
    QCheckBox *m_sizeMinCheck = nullptr;
    QDoubleSpinBox *m_sizeMinSpin = nullptr;
    QComboBox *m_sizeMinUnitCombo = nullptr;
    QCheckBox *m_sizeMaxCheck = nullptr;
    QDoubleSpinBox *m_sizeMaxSpin = nullptr;
    QComboBox *m_sizeMaxUnitCombo = nullptr;

    // Validation status label
    QLabel *m_validationErrorLabel = nullptr;

    QDialogButtonBox *m_buttonBox = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_clearButton = nullptr;
};
