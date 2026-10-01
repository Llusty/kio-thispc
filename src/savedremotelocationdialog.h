/*
 * Dialog for adding and renaming Saved Remote Locations.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <QDialog>
#include <QUrl>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

class SavedRemoteLocationDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode {
        Add,
        Rename
    };

    explicit SavedRemoteLocationDialog(Mode mode, const QUrl &prefillUrl = QUrl(),
                                       const QString &prefillName = QString(),
                                       const QString &existingId = QString(),
                                       QWidget *parent = nullptr);

    QString displayName() const;
    QUrl url() const;

private Q_SLOTS:
    void validateInputs();

private:
    Mode m_mode = Mode::Add;
    QString m_existingId;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_urlEdit = nullptr;
    QLabel *m_securityNotice = nullptr;
    QLabel *m_statusLabel = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
