/*
 * Implementation of dialog for adding and renaming Saved Remote Locations.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "savedremotelocationdialog.h"
#include "browsercommon.h"
#include "remoteurlhelper.h"
#include "savedremotelocation.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

SavedRemoteLocationDialog::SavedRemoteLocationDialog(Mode mode, const QUrl &prefillUrl,
                                                     const QString &prefillName,
                                                     const QString &existingId,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_existingId(existingId)
{
    setWindowTitle(mode == Mode::Add
        ? trLocal("Dodaj lokalizację zdalną", "Add Remote Location")
        : trLocal("Zmień nazwę lokalizacji zdalnej", "Rename Remote Location"));

    setModal(true);
    setMinimumWidth(440);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(12);

    auto *formLayout = new QFormLayout();
    formLayout->setSpacing(10);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(trLocal("np. Mój serwer NAS", "e.g. My NAS Server"));
    formLayout->addRow(trLocal("Nazwa:", "Name:"), m_nameEdit);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("sftp://alice@server.example/home/alice"));
    formLayout->addRow(trLocal("Lokalizacja:", "Location:"), m_urlEdit);

    rootLayout->addLayout(formLayout);

    m_securityNotice = new QLabel(this);
    m_securityNotice->setWordWrap(true);
    m_securityNotice->setStyleSheet(QStringLiteral("color: #7f8c8d; font-size: 11px;"));
    m_securityNotice->setText(trLocal(
        "Hasła nie są zapisywane w konfiguracji. W razie potrzeby poświadczenia zostaną zażądane podczas łączenia.",
        "Passwords are not saved in configuration. If required, credentials will be requested when connecting."));
    rootLayout->addWidget(m_securityNotice);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #c0392b; font-weight: bold; font-size: 11px;"));
    m_statusLabel->hide();
    rootLayout->addWidget(m_statusLabel);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (QPushButton *okBtn = m_buttonBox->button(QDialogButtonBox::Ok)) {
        okBtn->setText(mode == Mode::Add
            ? trLocal("Dodaj", "Add")
            : trLocal("Zapisz", "Save"));
    }
    if (QPushButton *cancelBtn = m_buttonBox->button(QDialogButtonBox::Cancel)) {
        cancelBtn->setText(trLocal("Anuluj", "Cancel"));
    }
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rootLayout->addWidget(m_buttonBox);

    if (m_mode == Mode::Rename) {
        m_urlEdit->setReadOnly(true);
        m_urlEdit->setEnabled(false);
        m_securityNotice->hide();
    }

    if (prefillUrl.isValid()) {
        const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(prefillUrl);
        m_urlEdit->setText(sanitized.toString());
        if (prefillName.isEmpty()) {
            m_nameEdit->setText(SavedRemoteLocationsStore::defaultDisplayName(sanitized));
        } else {
            m_nameEdit->setText(prefillName);
        }
    } else if (!prefillName.isEmpty()) {
        m_nameEdit->setText(prefillName);
    }

    connect(m_nameEdit, &QLineEdit::textChanged, this, &SavedRemoteLocationDialog::validateInputs);
    connect(m_urlEdit, &QLineEdit::textChanged, this, &SavedRemoteLocationDialog::validateInputs);

    validateInputs();
}

QString SavedRemoteLocationDialog::displayName() const
{
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty() && m_urlEdit) {
        return SavedRemoteLocationsStore::defaultDisplayName(url());
    }
    return name;
}

QUrl SavedRemoteLocationDialog::url() const
{
    const QString text = m_urlEdit ? m_urlEdit->text().trimmed() : QString();
    const QUrl raw = urlFromUserText(text);
    return RemoteUrlHelper::sanitizeUrl(raw);
}

void SavedRemoteLocationDialog::validateInputs()
{
    QPushButton *okBtn = m_buttonBox->button(QDialogButtonBox::Ok);
    if (!okBtn) return;

    if (m_mode == Mode::Rename) {
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) {
            m_statusLabel->setText(trLocal("Wprowadź nazwę.", "Please enter a name."));
            m_statusLabel->show();
            okBtn->setEnabled(false);
            return;
        }
        m_statusLabel->hide();
        okBtn->setEnabled(true);
        return;
    }

    const QString urlText = m_urlEdit->text().trimmed();
    if (urlText.isEmpty()) {
        m_statusLabel->setText(trLocal("Wprowadź adres lokalizacji zdalnej.", "Enter the remote location URL."));
        m_statusLabel->show();
        okBtn->setEnabled(false);
        return;
    }

    const QUrl rawUrl = urlFromUserText(urlText);
    const QString scheme = rawUrl.scheme().toLower();

    if (!RemoteUrlHelper::isRemoteScheme(scheme)) {
        m_statusLabel->setText(trLocal(
            "Nieobsługiwany protokół. Dozwolone: smb://, sftp://, ftp://, webdav://, webdavs://",
            "Unsupported protocol. Allowed: smb://, sftp://, ftp://, webdav://, webdavs://"));
        m_statusLabel->show();
        okBtn->setEnabled(false);
        return;
    }

    if (rawUrl.host().trimmed().isEmpty()) {
        m_statusLabel->setText(trLocal("Wprowadź poprawny adres hosta lub serwera.", "Enter a valid host or server address."));
        m_statusLabel->show();
        okBtn->setEnabled(false);
        return;
    }

    const QUrl sanitized = RemoteUrlHelper::sanitizeUrl(rawUrl);
    if (!RemoteUrlHelper::isValidRemoteUrl(sanitized)) {
        m_statusLabel->setText(trLocal("Adres URL jest nieprawidłowy.", "The URL is invalid."));
        m_statusLabel->show();
        okBtn->setEnabled(false);
        return;
    }

    // Check duplicates against existing store
    const auto &store = SavedRemoteLocationsStore::instance();
    for (const auto &existing : store.locations()) {
        if (existing.id != m_existingId && RemoteUrlHelper::isSameRemoteLocation(existing.url, sanitized)) {
            m_statusLabel->setText(trLocal(
                "Ta lokalizacja jest już zapisana jako \"%1\".",
                "This location is already saved as \"%1\".").arg(existing.displayName));
            m_statusLabel->show();
            okBtn->setEnabled(false);
            return;
        }
    }

    // If password was typed, show security notice
    if (!rawUrl.password().isEmpty()) {
        m_statusLabel->setText(trLocal(
            "Uwaga: Hasło w adresie URL zostanie usunięte. Hasła nie są zapisywane.",
            "Notice: Password in URL will be removed. Passwords are not saved."));
        m_statusLabel->show();
    } else {
        m_statusLabel->hide();
    }

    okBtn->setEnabled(true);
}
