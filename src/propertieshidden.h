/* Stage 5: KIO visibility is not a portable native filesystem attribute. */
#pragma once
#include "browsercommon.h"
#include "propertiescapabilities.h"
#include <KIO/ListJob>
#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QSizePolicy>

struct PropertiesHiddenState {
    bool ready = false;
    bool hidden = false;
    bool dotName = false;
    bool kioOverride = false;
    QString error;
};
class PropertiesHiddenPolicy {
public:
    static bool dotHidden(const QString &name) { return name.size() > 1 && name.startsWith(QLatin1Char('.')); }
    static PropertiesHiddenState fromEntry(const QUrl &url, const KIO::UDSEntry &entry) {
        PropertiesHiddenState result;
        result.ready = true;
        const auto name = url.fileName().isEmpty() ? entry.stringValue(KIO::UDSEntry::UDS_NAME) : url.fileName();
        result.dotName = dotHidden(name);
        result.kioOverride = entry.contains(KIO::UDSEntry::UDS_HIDDEN);
        result.hidden = result.kioOverride ? entry.numberValue(KIO::UDSEntry::UDS_HIDDEN) != 0 : result.dotName;
        return result;
    }
    static bool unixFilesystem(const QString &type) {
        return QStringList{QStringLiteral("ext2"), QStringLiteral("ext3"), QStringLiteral("ext4"),
            QStringLiteral("btrfs"), QStringLiteral("xfs"), QStringLiteral("tmpfs")}.contains(type.toLower());
    }
    static bool editable(const QUrl &url, const PropertiesTargetCapabilities &caps,
                         const PropertiesHiddenState &state, const QString &name) {
        return url.isLocalFile() && caps.isLocal && !caps.isRemote && caps.entryIdentity.valid
            && (caps.entryKind == PropertiesEntryKind::RegularFile || caps.entryKind == PropertiesEntryKind::Directory)
            && caps.symlinkPolicy == PropertiesSymlinkPolicy::NotSymlink
            && unixFilesystem(caps.fileSystemType) && state.ready && !state.kioOverride
            && validNewName(name) && name == name.trimmed() && !name.startsWith(QStringLiteral(".."))
            && name != QStringLiteral(".") && !QFileInfo(url.toLocalFile()).isRoot()
            && QFileInfo(url.toLocalFile()).absoluteDir().isReadable()
            && QFileInfo(QFileInfo(url.toLocalFile()).absolutePath()).isWritable()
            && QFileInfo(QFileInfo(url.toLocalFile()).absolutePath()).isExecutable()
            && !QStorageInfo(url.toLocalFile()).isReadOnly();
    }
    static QString toggledName(const QString &name, bool hidden) {
        if (hidden == dotHidden(name)) return name;
        return hidden ? QStringLiteral(".") + name : name.mid(1);
    }
};

// Same ListJob metadata as DirectoryListingCore, one parent level only.
// No local emulation for remote URLs; no retained directory-sized collection.
class PropertiesHiddenProvider final : public QObject {
    Q_OBJECT
public:
    explicit PropertiesHiddenProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~PropertiesHiddenProvider() override { cancel(); }
    void cancel() {
        ++m_generation;
        if (m_job) { auto job = m_job; m_job = nullptr; job->kill(KJob::Quietly); }
    }
    void load(const QUrl &url, const PropertiesTargetCapabilities &caps) {
        cancel();
        const auto generation = m_generation;
        if (!url.isValid() || url.fileName().isEmpty()) {
            PropertiesHiddenState result; result.error = trLocal("Stan niedostępny dla tego adresu.", "State unavailable for this address.");
            Q_EMIT ready(result); return;
        }
        if (url.isLocalFile() && (!caps.entryIdentity.valid
            || PropertiesCapabilityResolver::revalidate(caps) != PropertiesRevalidationResult::SameTarget)) {
            PropertiesHiddenState result; result.error = trLocal("Nie można potwierdzić tożsamości elementu.", "Cannot verify item identity.");
            Q_EMIT ready(result); return;
        }
        auto *job = KIO::listDir(url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash), KIO::HideProgressInfo);
        job->setUiDelegate(nullptr);
        m_job = job;
        connect(job, &KIO::ListJob::redirection, this, [this, job, generation](KIO::Job *, const QUrl &) {
            if (m_job != job || generation != m_generation) return;
            cancel(); PropertiesHiddenState result; result.error = trLocal("Backend przekierował adres; stan niepotwierdzony.", "Backend redirected the address; state unverified.");
            Q_EMIT ready(result); // Do not bind redirected metadata to the old identity.
        });
        connect(job, &KIO::ListJob::entries, this, [this, job, generation, url, caps](KIO::Job *, const KIO::UDSEntryList &entries) {
            if (m_job != job || generation != m_generation) return;
            for (const auto &entry : entries) {
                if (entry.stringValue(KIO::UDSEntry::UDS_NAME) != url.fileName()) continue;
                auto result = PropertiesHiddenPolicy::fromEntry(url, entry);
                if (url.isLocalFile() && PropertiesCapabilityResolver::revalidate(caps) != PropertiesRevalidationResult::SameTarget) {
                    result = {}; result.error = trLocal("Element zniknął lub został podmieniony.", "The item disappeared or was replaced.");
                }
                m_job = nullptr; job->kill(KJob::Quietly);
                Q_EMIT ready(result); return;
            }
        });
        connect(job, &KJob::result, this, [this, job, generation] {
            if (m_job != job || generation != m_generation) return;
            m_job = nullptr;
            PropertiesHiddenState result;
            result.error = job->error() ? trLocal("Nie udało się odczytać stanu ukrycia.", "Could not read hidden state.")
                : trLocal("Element nie występuje w listingu backendu.", "The item is absent from the backend listing.");
            Q_EMIT ready(result);
        });
    }
Q_SIGNALS:
    void ready(const PropertiesHiddenState &state);
private:
    quint64 m_generation = 0;
    QPointer<KIO::ListJob> m_job;
};

class PropertiesHiddenWidget final : public QWidget {
    Q_OBJECT
public:
    PropertiesHiddenWidget(const QUrl &url, const PropertiesTargetCapabilities &caps, QLineEdit *name, QWidget *parent = nullptr)
        : QWidget(parent), m_url(url), m_caps(caps), m_name(name) {
        setObjectName(QStringLiteral("propertiesHidden"));
        auto *form = new QFormLayout(this); form->setContentsMargins(0,0,0,0);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setSizeConstraint(QLayout::SetMinimumSize);
        // Preserve wrapped row heights when the enclosing General form lays us out.
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        m_visibility = new QLabel(this); m_visibility->setObjectName(QStringLiteral("propertiesHiddenState")); m_visibility->setWordWrap(true);
        form->addRow(trLocal("Ukryty w ThisPC (KIO):", "Hidden in ThisPC (KIO):"), m_visibility);
        m_dot = new QCheckBox(trLocal("Ukryj", "Hide"), this);
        m_dot->setObjectName(QStringLiteral("propertiesHiddenDotName")); form->addRow(m_dot);
        m_native = new QLabel(this); m_native->setWordWrap(true); m_native->setObjectName(QStringLiteral("propertiesNativeHiddenState"));
        form->addRow(trLocal("Natywny atrybut hidden:", "Native hidden attribute:"), m_native);
        m_note = new QLabel(this); m_note->setWordWrap(true); form->addRow(m_note);
        for (auto *label : {m_visibility, m_native, m_note}) {
            QSizePolicy labelPolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
            labelPolicy.setHeightForWidth(true);
            label->setSizePolicy(labelPolicy);
        }
        m_provider = new PropertiesHiddenProvider(this);
        connect(m_provider, &PropertiesHiddenProvider::ready, this, [this](const auto &state) { m_state = state; render(); });
        connect(m_dot, &QCheckBox::toggled, this, [this](bool hidden) {
            if (!m_dot->isEnabled() || !PropertiesHiddenPolicy::editable(m_url,m_caps,m_state,m_name->text())) { render(); return; }
            m_name->setText(PropertiesHiddenPolicy::toggledName(m_name->text(),hidden));
        });
        connect(m_name, &QLineEdit::textChanged, this, [this] { render(); });
        reload(url,caps);
    }
    void reload(const QUrl &url, const PropertiesTargetCapabilities &caps) {
        m_url=url; m_caps=caps; m_state={}; render(); m_provider->load(url,caps);
    }
    bool loaded() const { return m_state.ready; }
private:
    void render() {
        const QSignalBlocker blocker(m_dot);
        m_dot->setChecked(PropertiesHiddenPolicy::dotHidden(m_name->text()));
        m_dot->setEnabled(m_name->isEnabled() && !m_name->isReadOnly()
            && PropertiesHiddenPolicy::editable(m_url,m_caps,m_state,m_name->text()));
        m_visibility->setText(!m_state.error.isEmpty() ? m_state.error : !m_state.ready
            ? trLocal("Nieznany / odczytywanie", "Unknown / loading")
            : (m_state.hidden ? trLocal("Tak", "Yes") : trLocal("Nie", "No"))
                + (m_state.kioOverride ? trLocal(" — stan backendu", " — backend state") : trLocal(" — reguła nazwy KIO", " — KIO name rule")));
        m_native->setText(m_url.isLocalFile() && PropertiesHiddenPolicy::unixFilesystem(m_caps.fileSystemType)
            ? trLocal("Nie dotyczy — semantyka nazwy Unix", "Not applicable — Unix name semantics")
            : trLocal("Nieznany — brak potwierdzonego API odczytu natywnej flagi", "Unknown — no verified native flag reading API"));
        m_note->setText(m_dot->isEnabled()
            ? trLocal("Zmiana kropki zmienia nazwę. Zapis przez Zastosuj/OK; Anuluj odrzuca zmianę. Nie zmienia natywnej flagi.",
                      "Changing the dot changes the name. Apply/OK saves; Cancel discards. It does not change a native flag.")
            : trLocal("Stan tylko do odczytu. Bez bezpiecznej semantyki nie zmieniamy flagi ani nie emulujemy jej nazwą.",
                      "Read-only state. Without safe semantics, no flag change or name emulation is offered."));
        // Text changes invalidate the inner form's height-for-width cache.
        // Propagate that change to General before its next geometry pass; the
        // child minimum-size constraint alone can resize us over later rows.
        layout()->invalidate();
        updateGeometry();
        if (parentWidget() && parentWidget()->layout())
            parentWidget()->layout()->invalidate();
    }
    QUrl m_url; PropertiesTargetCapabilities m_caps; QLineEdit *m_name;
    PropertiesHiddenState m_state; PropertiesHiddenProvider *m_provider;
    QLabel *m_visibility, *m_native, *m_note; QCheckBox *m_dot;
};
