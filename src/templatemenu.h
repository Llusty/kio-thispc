/*
 * User template discovery for the existing New menu.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "browsercommon.h"
#include <KFileItem>
#include <KIO/ListJob>
#include <QMenu>
#include <QPointer>
#include <QStandardPaths>
#include <utility>

class TemplateMenu final : public QMenu
{
    Q_OBJECT
public:
    explicit TemplateMenu(QWidget *parent = nullptr)
        : QMenu(trLocal("Szablony", "Templates"), parent)
    {
        setIcon(themedIcon(QStringLiteral("folder-templates"), QStringLiteral("folder")));
        showPlaceholder(trLocal("Brak szablonów", "No templates found"));
        connect(this, &QMenu::aboutToShow, this, &TemplateMenu::reload);
    }

    ~TemplateMenu() override
    {
        if (m_job) m_job->kill();
    }

Q_SIGNALS:
    void templateSelected(const QUrl &source);

private:
    void showPlaceholder(const QString &text)
    {
        clear();
        addAction(text)->setEnabled(false);
    }

    void reload()
    {
        if (m_job) {
            disconnect(m_job, nullptr, this, nullptr);
            m_job->kill();
            m_job = nullptr;
        }
        m_names.clear();
        const QString path = QStandardPaths::writableLocation(QStandardPaths::TemplatesLocation);
        setToolTip(path);
        // XDG uses HOME to indicate a disabled user directory. Never list
        // the whole home directory as templates in that configuration.
        if (path.isEmpty() || QDir::cleanPath(path) == QDir::homePath()) {
            showPlaceholder(trLocal("Brak szablonów", "No templates found"));
            return;
        }

        showPlaceholder(trLocal("Wczytywanie szablonów…", "Loading templates…"));
        const QUrl directory = QUrl::fromLocalFile(path);
        // A user directory can be on a slow mount. Keep discovery off the
        // UI thread and refresh on each opening without a permanent watcher.
        m_job = KIO::listDir(directory, KIO::HideProgressInfo);
        m_job->setUiDelegate(nullptr);
        connect(m_job, &KIO::ListJob::entries, this,
                [this, directory](KIO::Job *, const KIO::UDSEntryList &entries) {
            for (const auto &entry : entries) {
                const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
                const KFileItem item(entry, childUrlWithName(directory, name), true);
                // Stage 1B lists visible regular files only. Desktop files
                // are copied verbatim, never interpreted as KDE recipes.
                if (!name.startsWith(QLatin1Char('.')) && validNewName(name)
                    && S_ISREG(entry.numberValue(KIO::UDSEntry::UDS_FILE_TYPE))
                    && !item.isLink() && item.isReadable()) {
                    m_names.append(name);
                }
            }
        });
        connect(m_job, &KJob::result, this, [this, directory](KJob *job) {
            m_job = nullptr;
            if (job->error() && job->error() != KIO::ERR_DOES_NOT_EXIST) {
                showPlaceholder(trLocal("Nie można odczytać szablonów", "Could not read templates"));
                return;
            }
            if (job->error() || m_names.isEmpty()) {
                showPlaceholder(trLocal("Brak szablonów", "No templates found"));
                return;
            }
            clear();
            m_names.sort(Qt::CaseInsensitive);
            for (const QString &name : std::as_const(m_names)) {
                const QUrl source = childUrlWithName(directory, name);
                auto *action = addAction(themedIcon(QStringLiteral("document-new")),
                                         QString(name).replace(QLatin1Char('&'), QStringLiteral("&&")));
                action->setData(source);
                connect(action, &QAction::triggered, this, [this, source] {
                    Q_EMIT templateSelected(source);
                });
            }
        });
    }

    QPointer<KIO::ListJob> m_job;
    QStringList m_names;
};
