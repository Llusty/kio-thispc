/*
 * Isolated, Dolphin-inspired compact conflict-resolution dialogs for FOLDER-MERGE-01 (Stage 3A).
 * - LocalFolderMergeConflictDialog: 2-column Source/Destination comparison, Merge / Rename / Skip / Cancel.
 * - LocalFileConflictResolutionDialog: 2-column comparison, MIME icons, Overwrite / Rename / Skip / Cancel,
 *   collapsible technical Details section, strictly-scoped ApplyToAll.
 * - Pure presentation + explicit decision model; performs ZERO filesystem I/O.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "browsercommon.h"
#include "localmergeplan.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeDatabase>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
#include <QString>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

struct LocalMergeBackendCapabilities
{
    bool canMergeDirectories = true;
    bool canCreateNewFiles = true;
    bool canRenameConflictingCopies = true;
    bool canSafeOverwrite = false;
    QString filesystemLabel;
    QString overwriteUnavailableReason;
};

struct LocalMergeConflictProgress
{
    qsizetype currentIndex = 0;
    qsizetype totalConflicts = 0;
};

struct LocalFolderConflictDecision
{
    LocalMergeApprovedAction action = LocalMergeApprovedAction::Abort;
    LocalMergeApplyPolicy applyPolicy = LocalMergeApplyPolicy::None;
    QString targetFolderName;
    bool accepted = false;
};

struct LocalFileConflictDecision
{
    LocalMergeApprovedAction action = LocalMergeApprovedAction::Abort;
    LocalMergeApplyPolicy applyPolicy = LocalMergeApplyPolicy::None;
    QString targetFileName;
    bool accepted = false;
};

struct LocalMergeEntryResolutionView
{
    QString originalConflictDestination;
    QString approvedNewName;
    QString effectiveDestination;
    bool needsRevalidation = false;
};

inline QString effectiveMergeDestinationPath(const LocalMergePlanEntry &entry)
{
    if (entry.approvedAction == LocalMergeApprovedAction::Rename
        && !entry.decisionBinding.targetFileName.isEmpty()) {
        return QDir::cleanPath(
            QDir(QFileInfo(entry.destinationPath).path()).filePath(entry.decisionBinding.targetFileName));
    }
    return QDir::cleanPath(entry.destinationPath);
}

inline LocalMergeEntryResolutionView resolveEntryDestinationView(
    const LocalMergePlanEntry &entry,
    bool needsRevalidation = false)
{
    LocalMergeEntryResolutionView view;
    view.originalConflictDestination = QDir::cleanPath(entry.destinationPath);
    view.approvedNewName = entry.decisionBinding.targetFileName;
    view.effectiveDestination = effectiveMergeDestinationPath(entry);
    view.needsRevalidation = needsRevalidation;
    return view;
}

namespace LocalMergeDialogDetail {

inline bool isSafeSingleComponent(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    return !name.contains(QLatin1Char('/'))
        && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char('\0'));
}

inline bool hasCaseInsensitiveCollision(const QString &candidate, const QSet<QString> &existingNames)
{
    const QString folded = candidate.toCaseFolded();
    for (const QString &existing : existingNames) {
        if (existing == candidate || existing.toCaseFolded() == folded) {
            return true;
        }
    }
    return false;
}

inline QString suggestRenamedFile(const QString &originalName, const QSet<QString> &existingNames, bool polish)
{
    const qsizetype dot = originalName.lastIndexOf(QLatin1Char('.'));
    const bool hasExt = dot > 0 && dot < originalName.size() - 1;
    const QString stem = hasExt ? originalName.left(dot) : originalName;
    const QString ext = hasExt ? originalName.mid(dot) : QString();
    const QString copyTag = polish ? QStringLiteral("kopia") : QStringLiteral("copy");

    const QString first = QStringLiteral("%1 (%2)%3").arg(stem, copyTag, ext);
    if (!hasCaseInsensitiveCollision(first, existingNames)) {
        return first;
    }
    for (int i = 2; i < 1000; ++i) {
        const QString cand = QStringLiteral("%1 (%2 %3)%4").arg(stem, copyTag).arg(i).arg(ext);
        if (!hasCaseInsensitiveCollision(cand, existingNames)) {
            return cand;
        }
    }
    return {};
}

inline QString suggestRenamedFolder(const QString &originalName, const QSet<QString> &existingNames, bool polish)
{
    const QString copyTag = polish ? QStringLiteral("kopia") : QStringLiteral("copy");
    const QString first = QStringLiteral("%1 (%2)").arg(originalName, copyTag);
    if (!hasCaseInsensitiveCollision(first, existingNames)) {
        return first;
    }
    for (int i = 2; i < 1000; ++i) {
        const QString cand = QStringLiteral("%1 (%2 %3)").arg(originalName, copyTag).arg(i);
        if (!hasCaseInsensitiveCollision(cand, existingNames)) {
            return cand;
        }
    }
    return {};
}

// Compact path label that elides in the middle without overflowing the window,
// while exposing the complete path in toolTip(), selection, and fullPath().
class ElidedPathLabel final : public QLabel
{
public:
    explicit ElidedPathLabel(const QString &fullPath, QWidget *parent = nullptr)
        : QLabel(parent)
        , m_fullPath(fullPath)
    {
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setToolTip(m_fullPath);
        setAccessibleDescription(m_fullPath);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumWidth(120);
        updateElision();
    }

    QString fullPath() const { return m_fullPath; }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateElision();
    }

private:
    void updateElision()
    {
        const int avail = qMax(140, width() > 20 ? width() - 4 : 210);
        setText(fontMetrics().elidedText(m_fullPath, Qt::ElideMiddle, avail));
    }

    QString m_fullPath;
};

inline QIcon iconForMergeEntry(LocalMergeObjectKind kind, const QString &name, const QStyle *style)
{
    if (kind == LocalMergeObjectKind::Directory) {
        return QIcon::fromTheme(QStringLiteral("folder"),
                                style ? style->standardIcon(QStyle::SP_DirIcon) : QIcon());
    }
    if (kind == LocalMergeObjectKind::SymbolicLink) {
        return QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"),
                                style ? style->standardIcon(QStyle::SP_FileLinkIcon) : QIcon());
    }
    static QMimeDatabase mimeDb;
    const QMimeType mime = mimeDb.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    const QIcon docFallback = QIcon::fromTheme(
        QStringLiteral("text-x-generic"),
        style ? style->standardIcon(QStyle::SP_FileIcon) : QIcon());

    const QString iconName = mime.iconName();
    if (!iconName.isEmpty()
        && iconName != QLatin1String("unknown")
        && iconName != QLatin1String("application-octet-stream")
        && QIcon::hasThemeIcon(iconName)) {
        return QIcon::fromTheme(iconName, docFallback);
    }
    const QString genericName = mime.genericIconName();
    if (!genericName.isEmpty()
        && genericName != QLatin1String("unknown")
        && QIcon::hasThemeIcon(genericName)) {
        return QIcon::fromTheme(genericName, docFallback);
    }
    return docFallback;
}

inline QString formatTimestampFromStat(const struct stat &st, bool valid, const QLocale &locale)
{
    if (!valid || st.st_mtim.tv_sec <= 0) {
        return QStringLiteral("—");
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(st.st_mtim.tv_sec);
    return dt.isValid() ? locale.toString(dt, QLocale::ShortFormat) : QStringLiteral("—");
}

inline QWidget *buildComparisonCard(
    const QString &roleTitle,
    const QIcon &icon,
    const QString &itemName,
    const QString &sizeText,
    const QString &dateText,
    const QString &fullPath,
    ElidedPathLabel **outPathLabel,
    QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setFrameShape(QFrame::StyledPanel);
    auto *vbox = new QVBoxLayout(card);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(4);

    auto *roleLbl = new QLabel(roleTitle, card);
    QFont rf = roleLbl->font();
    rf.setBold(true);
    vbox->addWidget(roleLbl);

    auto *topRow = new QHBoxLayout();
    topRow->setSpacing(8);
    auto *iconLbl = new QLabel(card);
    iconLbl->setPixmap(icon.pixmap(36, 36));
    iconLbl->setFixedSize(36, 36);
    topRow->addWidget(iconLbl, 0, Qt::AlignTop);

    auto *metaCol = new QVBoxLayout();
    metaCol->setSpacing(2);
    auto *nameLbl = new QLabel(itemName, card);
    QFont nf = nameLbl->font();
    nf.setBold(true);
    nameLbl->setFont(nf);
    nameLbl->setWordWrap(true);
    metaCol->addWidget(nameLbl);

    const bool hasSize = !sizeText.isEmpty() && sizeText != QStringLiteral("—");
    const bool hasDate = !dateText.isEmpty() && dateText != QStringLiteral("—");
    if (hasSize || hasDate) {
        const QString metaText = (hasSize && hasDate)
            ? QStringLiteral("%1  •  %2").arg(sizeText, dateText)
            : (hasSize ? sizeText : dateText);
        auto *sizeDateLbl = new QLabel(metaText, card);
        sizeDateLbl->setObjectName(QStringLiteral("comparisonMetaLabel"));
        sizeDateLbl->setWordWrap(true);
        metaCol->addWidget(sizeDateLbl);
    }
    topRow->addLayout(metaCol, 1);
    vbox->addLayout(topRow);

    auto *pathLbl = new ElidedPathLabel(fullPath, card);
    vbox->addWidget(pathLbl);
    if (outPathLabel) {
        *outPathLabel = pathLbl;
    }
    return card;
}

inline QPlainTextEdit *makeDetailsPathsView(const QString &text, QWidget *parent)
{
    auto *edit = new QPlainTextEdit(text, parent);
    edit->setReadOnly(true);
    edit->setWordWrapMode(QTextOption::WrapAnywhere);
    edit->setFrameShape(QFrame::NoFrame);
    edit->viewport()->setAutoFillBackground(false);
    edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    edit->setFixedHeight(72);
    return edit;
}

inline void applyDetailsToggleGeometry(
    QDialog *dlg,
    QFrame *detailsFrame,
    bool expanded,
    QSize &savedCompactSize,
    bool userResized,
    bool &programmaticResize)
{
    if (!dlg || !detailsFrame) return;
    if (expanded) {
        if (!userResized) {
            savedCompactSize = dlg->size();
        }
        detailsFrame->setVisible(true);
        if (dlg->layout()) dlg->layout()->activate();
        const QSize hint = dlg->sizeHint();
        const int targetWidth = qMax(dlg->width(), savedCompactSize.isValid() ? savedCompactSize.width() : hint.width());
        int maxH = 680;
        if (const QScreen *scr = dlg->screen()) {
            maxH = static_cast<int>(scr->availableGeometry().height() * 0.85);
        }
        const int targetHeight = qMin(qMax(hint.height(), dlg->height()), maxH);
        programmaticResize = true;
        dlg->resize(targetWidth, targetHeight);
        programmaticResize = false;
    } else {
        detailsFrame->setVisible(false);
        if (dlg->layout()) dlg->layout()->activate();
        if (!userResized && savedCompactSize.isValid()) {
            programmaticResize = true;
            dlg->resize(savedCompactSize);
            programmaticResize = false;
        }
    }
}

}  // namespace LocalMergeDialogDetail

class LocalFolderMergeConflictDialog final : public QDialog
{
public:
    explicit LocalFolderMergeConflictDialog(
        const LocalMergePlanEntry &entry,
        const QSet<QString> &existingSiblingNames = {},
        bool isSelfCopy = false,
        const QLocale &locale = QLocale(),
        QWidget *parent = nullptr,
        qsizetype remainingFolderConflicts = 0,
        LocalMergeConflictProgress progress = {})
        : QDialog(parent)
        , m_entry(entry)
        , m_existingSiblings(existingSiblingNames)
        , m_isSelfCopy(isSelfCopy || entry.classification == LocalMergeEntryClassification::SameObject)
        , m_locale(locale)
        , m_polish(locale.language() == QLocale::Polish)
    {
        setModal(true);
        setMinimumWidth(480);
        setWindowTitle(trText("Konflikt folderów", "Folder Conflict"));

        const QString folderName = QFileInfo(m_entry.relativePath).fileName().isEmpty()
            ? m_entry.relativePath
            : QFileInfo(m_entry.relativePath).fileName();
        m_existingSiblings.insert(folderName);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        // Compact header + discreet conflict counter
        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);
        auto *iconLabel = new QLabel(this);
        const QIcon warnIcon = style()->standardIcon(
            m_isSelfCopy ? QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxQuestion);
        iconLabel->setPixmap(warnIcon.pixmap(28, 28));
        headerRow->addWidget(iconLabel, 0, Qt::AlignTop);

        auto *titleBox = new QVBoxLayout();
        titleBox->setSpacing(2);
        m_titleLabel = new QLabel(
            m_isSelfCopy
                ? trText("Nie można scalić folderu „%1” z samym sobą",
                         "Cannot merge folder “%1” with itself").arg(folderName)
                : trText("Folder „%1” już istnieje",
                         "Folder “%1” already exists").arg(folderName),
            this);
        QFont titleFont = m_titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
        m_titleLabel->setFont(titleFont);
        m_titleLabel->setWordWrap(true);
        titleBox->addWidget(m_titleLabel);

        const QString progressText = (progress.totalConflicts > 0 && progress.currentIndex > 0)
            ? trText("Konflikt %1 z %2  •  %3", "Conflict %1 of %2  •  %3")
                  .arg(progress.currentIndex)
                  .arg(progress.totalConflicts)
                  .arg(m_entry.relativePath)
            : m_entry.relativePath;
        m_progressLabel = new QLabel(progressText, this);
        m_progressLabel->setWordWrap(true);
        titleBox->addWidget(m_progressLabel);
        headerRow->addLayout(titleBox, 1);
        root->addLayout(headerRow);

        m_subtitleLabel = new QLabel(
            m_isSelfCopy
                ? trText("Źródło i cel wskazują ten sam folder. Scalanie i zmiana nazwy są zablokowane — pomiń folder lub anuluj.",
                         "Source and destination refer to the same folder. Merge and Rename are blocked — skip folder or cancel.")
                : trText("Scalanie łączy zawartość folderów bez automatycznego zastępowania istniejących plików.",
                         "Merging combines folder contents without automatically replacing existing files."),
            this);
        m_subtitleLabel->setWordWrap(true);
        root->addWidget(m_subtitleLabel);

        // Two-column Dolphin-inspired Source / Destination comparison
        auto *cmpRow = new QHBoxLayout();
        cmpRow->setSpacing(8);
        const QIcon srcIcon = LocalMergeDialogDetail::iconForMergeEntry(m_entry.sourceKind, folderName, style());
        const QIcon dstIcon = LocalMergeDialogDetail::iconForMergeEntry(m_entry.destinationKind, folderName, style());
        const QString srcSize = m_entry.size > 0
            ? formatFileSize(static_cast<qint64>(m_entry.size), false)
            : QStringLiteral("—");
        const QString dstSize = m_entry.destinationSize > 0
            ? formatFileSize(static_cast<qint64>(m_entry.destinationSize), false)
            : QStringLiteral("—");

        cmpRow->addWidget(LocalMergeDialogDetail::buildComparisonCard(
            trText("Element źródłowy", "Source item"),
            srcIcon, folderName, srcSize,
            LocalMergeDialogDetail::formatTimestampFromStat(m_entry.sourceStat, m_entry.sourceStatValid, m_locale),
            m_entry.sourcePath, &m_sourcePathLabel, this), 1);

        cmpRow->addWidget(LocalMergeDialogDetail::buildComparisonCard(
            trText("Element docelowy", "Destination item"),
            dstIcon, folderName, dstSize,
            LocalMergeDialogDetail::formatTimestampFromStat(m_entry.destinationStat, m_entry.destinationStatValid, m_locale),
            m_entry.destinationPath, &m_destPathLabel, this), 1);
        root->addLayout(cmpRow);

        // Understated, always-visible Rename row (not auto-focused on open)
        auto *renameBox = new QVBoxLayout();
        renameBox->setSpacing(2);
        auto *renameRow = new QHBoxLayout();
        renameRow->setSpacing(6);
        auto *renamePrompt = new QLabel(trText("Nowa nazwa:", "New name:"), this);
        m_renameEdit = new QLineEdit(this);
        m_renameEdit->setText(LocalMergeDialogDetail::suggestRenamedFolder(folderName, m_existingSiblings, m_polish));
        m_suggestButton = new QPushButton(trText("Zaproponuj nazwę", "Suggest name"), this);
        m_suggestButton->setAutoDefault(false);
        connect(m_suggestButton, &QPushButton::clicked, this, [this, folderName] {
            m_renameEdit->setText(LocalMergeDialogDetail::suggestRenamedFolder(folderName, m_existingSiblings, m_polish));
        });
        renameRow->addWidget(renamePrompt);
        renameRow->addWidget(m_renameEdit, 1);
        renameRow->addWidget(m_suggestButton);
        renameBox->addLayout(renameRow);

        m_validationLabel = new QLabel(this);
        m_validationLabel->setWordWrap(true);
        m_validationLabel->setVisible(false);
        renameBox->addWidget(m_validationLabel);
        root->addLayout(renameBox);

        // Independent folder ApplyToAll
        const bool folderApplyEligible =
            !m_isSelfCopy
            && m_entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
            && m_entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch
            && remainingFolderConflicts > 0;
        m_applyToAllCheck = new QCheckBox(
            trText("Zastosuj do wszystkich pozostałych folderów w tym zadaniu (bez nadpisywania plików)",
                   "Apply to all remaining folders in this task (without overwriting files)"),
            this);
        m_applyToAllCheck->setEnabled(folderApplyEligible);
        m_applyToAllCheck->setVisible(folderApplyEligible);
        root->addWidget(m_applyToAllCheck);

        // Collapsible Details section (WrapAnywhere QPlainTextEdit prevents horizontal bloat)
        m_detailsToggle = new QToolButton(this);
        m_detailsToggle->setText(trText("Szczegóły ▸", "Details ▸"));
        m_detailsToggle->setCheckable(true);
        m_detailsToggle->setAutoRaise(true);
        root->addWidget(m_detailsToggle, 0, Qt::AlignLeft);

        m_detailsFrame = new QFrame(this);
        m_detailsFrame->setFrameShape(QFrame::StyledPanel);
        m_detailsFrame->setVisible(false);
        auto *detLay = new QVBoxLayout(m_detailsFrame);
        detLay->setContentsMargins(8, 6, 8, 6);
        detLay->addWidget(LocalMergeDialogDetail::makeDetailsPathsView(
            trText("Pełne źródło: %1\nPełny cel: %2", "Full source: %1\nFull destination: %2")
                .arg(m_entry.sourcePath, m_entry.destinationPath),
            m_detailsFrame));
        root->addWidget(m_detailsFrame);

        connect(m_detailsToggle, &QToolButton::toggled, this, [this](bool on) {
            m_detailsToggle->setText(on ? trText("Szczegóły ▾", "Details ▾") : trText("Szczegóły ▸", "Details ▸"));
            LocalMergeDialogDetail::applyDetailsToggleGeometry(
                this, m_detailsFrame, on, m_savedCompactSize, m_userResized, m_programmaticResize);
        });

        // Compact 4-decision bar
        auto *btnRow = new QHBoxLayout();
        btnRow->setSpacing(6);
        btnRow->addStretch(1);

        const bool mergePermitted =
            !m_isSelfCopy && m_entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate;
        m_mergeButton = new QPushButton(trText("Scal zawartość folderów", "Merge folder contents"), this);
        m_mergeButton->setEnabled(mergePermitted);
        m_mergeButton->setDefault(mergePermitted);
        m_mergeButton->setAutoDefault(mergePermitted);
        if (!mergePermitted) {
            m_mergeButton->setToolTip(trText(
                "Scalanie folderu z samym sobą jest zablokowane ze względów bezpieczeństwa.",
                "Merging a folder with itself is blocked for safety reasons."));
        }
        connect(m_mergeButton, &QPushButton::clicked, this, [this] {
            m_decision.action = LocalMergeApprovedAction::MergeDirectory;
            m_decision.applyPolicy = m_applyToAllCheck->isChecked()
                ? LocalMergeApplyPolicy::MergeAllFolders
                : LocalMergeApplyPolicy::None;
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_mergeButton);

        m_renameButton = new QPushButton(trText("Zmień nazwę", "Rename"), this);
        m_renameButton->setAutoDefault(false);
        if (m_isSelfCopy) {
            m_renameButton->setEnabled(false);
            m_renameButton->setToolTip(trText(
                "Zmiana nazwy folderu wskazującego samego siebie jest zablokowana, aby uniknąć utworzenia pustego folderu.",
                "Renaming a self-copy folder is blocked to prevent creating an empty folder without its contents."));
            m_renameEdit->setEnabled(false);
            m_suggestButton->setEnabled(false);
        }
        connect(m_renameButton, &QPushButton::clicked, this, [this] {
            if (m_isSelfCopy || !validateRenameInput()) {
                return;
            }
            m_decision.action = LocalMergeApprovedAction::Rename;
            m_decision.applyPolicy = LocalMergeApplyPolicy::None;
            m_decision.targetFolderName = m_renameEdit->text();
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_renameButton);

        m_skipButton = new QPushButton(trText("Pomiń folder", "Skip folder"), this);
        m_skipButton->setAutoDefault(false);
        connect(m_skipButton, &QPushButton::clicked, this, [this] {
            m_decision.action = LocalMergeApprovedAction::Skip;
            m_decision.applyPolicy = m_applyToAllCheck->isChecked()
                ? LocalMergeApplyPolicy::SkipAllFolders
                : LocalMergeApplyPolicy::None;
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_skipButton);

        m_cancelButton = new QPushButton(trText("Anuluj", "Cancel"), this);
        m_cancelButton->setAutoDefault(false);
        connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        btnRow->addWidget(m_cancelButton);

        root->addLayout(btnRow);

        if (m_isSelfCopy) {
            m_skipButton->setDefault(true);
            m_skipButton->setAutoDefault(true);
            m_skipButton->setFocus();
        } else if (mergePermitted) {
            m_mergeButton->setFocus();
        } else {
            m_skipButton->setDefault(true);
            m_skipButton->setAutoDefault(true);
            m_skipButton->setFocus();
        }

        connect(m_renameEdit, &QLineEdit::textChanged, this, [this] { validateRenameInput(); });
        connect(m_renameEdit, &QLineEdit::returnPressed, this, [this] {
            if (validateRenameInput()) {
                m_renameButton->click();
            }
        });
        validateRenameInput();
    }

    LocalFolderConflictDecision decision() const { return m_decision; }
    QPushButton *mergeButton() const { return m_mergeButton; }
    QPushButton *renameButton() const { return m_renameButton; }
    QPushButton *skipButton() const { return m_skipButton; }
    QPushButton *cancelButton() const { return m_cancelButton; }
    QPushButton *suggestButton() const { return m_suggestButton; }
    QLineEdit *renameLineEdit() const { return m_renameEdit; }
    QCheckBox *applyToAllCheckBox() const { return m_applyToAllCheck; }
    QLabel *progressLabel() const { return m_progressLabel; }
    QLabel *validationLabel() const { return m_validationLabel; }
    QLabel *titleLabel() const { return m_titleLabel; }
    LocalMergeDialogDetail::ElidedPathLabel *sourcePathLabel() const { return m_sourcePathLabel; }
    LocalMergeDialogDetail::ElidedPathLabel *destinationPathLabel() const { return m_destPathLabel; }
    QFrame *detailsFrame() const { return m_detailsFrame; }
    QToolButton *detailsToggleButton() const { return m_detailsToggle; }

    void reject() override
    {
        m_decision = LocalFolderConflictDecision{LocalMergeApprovedAction::Abort, LocalMergeApplyPolicy::None, {}, false};
        QDialog::reject();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        if (!m_programmaticResize && isVisible() && m_detailsToggle && !m_detailsToggle->isChecked()) {
            m_userResized = true;
        }
    }

private:
    QString trText(const char *pl, const char *en) const
    {
        return m_polish ? QString::fromUtf8(pl) : QString::fromUtf8(en);
    }

    bool validateRenameInput()
    {
        if (m_isSelfCopy) {
            m_renameButton->setEnabled(false);
            m_validationLabel->setVisible(false);
            return false;
        }
        const QString name = m_renameEdit->text();
        QString err;
        if (!LocalMergeDialogDetail::isSafeSingleComponent(name)) {
            err = trText("Nazwa musi być pojedynczym bezpiecznym elementem (bez /, \\ oraz ..).",
                         "Name must be a single safe component (without /, \\ or ..).");
        } else if (LocalMergeDialogDetail::hasCaseInsensitiveCollision(name, m_existingSiblings)) {
            err = trText("Ta nazwa koliduje z istniejącym elementem w folderze docelowym.",
                         "This name collides with an existing item in the destination folder.");
        }
        const bool ok = err.isEmpty();
        m_renameButton->setEnabled(ok);
        m_validationLabel->setText(err);
        m_validationLabel->setVisible(!ok);
        return ok;
    }

    LocalMergePlanEntry m_entry;
    QSet<QString> m_existingSiblings;
    bool m_isSelfCopy = false;
    QLocale m_locale;
    bool m_polish = false;
    LocalFolderConflictDecision m_decision;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    LocalMergeDialogDetail::ElidedPathLabel *m_sourcePathLabel = nullptr;
    LocalMergeDialogDetail::ElidedPathLabel *m_destPathLabel = nullptr;
    QLineEdit *m_renameEdit = nullptr;
    QLabel *m_validationLabel = nullptr;
    QCheckBox *m_applyToAllCheck = nullptr;
    QToolButton *m_detailsToggle = nullptr;
    QFrame *m_detailsFrame = nullptr;
    QSize m_savedCompactSize;
    bool m_userResized = false;
    bool m_programmaticResize = false;
    QPushButton *m_suggestButton = nullptr;
    QPushButton *m_mergeButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_skipButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};

class LocalFileConflictResolutionDialog final : public QDialog
{
public:
    explicit LocalFileConflictResolutionDialog(
        const LocalMergePlanEntry &entry,
        const LocalMergeBackendCapabilities &capabilities,
        qsizetype remainingExactFileConflicts = 0,
        const QSet<QString> &existingSiblingNames = {},
        const QLocale &locale = QLocale(),
        QWidget *parent = nullptr,
        LocalMergeConflictProgress progress = {})
        : QDialog(parent)
        , m_entry(entry)
        , m_capabilities(capabilities)
        , m_remainingExactConflicts(remainingExactFileConflicts)
        , m_existingSiblings(existingSiblingNames)
        , m_locale(locale)
        , m_polish(locale.language() == QLocale::Polish)
    {
        setModal(true);
        setMinimumWidth(500);
        setWindowTitle(trText("Konflikt plików", "File Conflict"));

        const QString fileName = QFileInfo(m_entry.relativePath).fileName().isEmpty()
            ? m_entry.relativePath
            : QFileInfo(m_entry.relativePath).fileName();
        m_existingSiblings.insert(fileName);

        const bool isExactRegularFileConflict =
            m_entry.classification == LocalMergeEntryClassification::FileConflict
            && m_entry.sourceKind == LocalMergeObjectKind::RegularFile
            && m_entry.destinationKind == LocalMergeObjectKind::RegularFile
            && m_entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch;
        const bool isSymlinkConflict =
            m_entry.classification == LocalMergeEntryClassification::DestinationSymlinkConflict
            || m_entry.classification == LocalMergeEntryClassification::BrokenSymlink
            || m_entry.sourceKind == LocalMergeObjectKind::SymbolicLink
            || m_entry.destinationKind == LocalMergeObjectKind::SymbolicLink;
        const bool isTypeConflict =
            m_entry.classification == LocalMergeEntryClassification::TypeConflictFileOverDir
            || m_entry.classification == LocalMergeEntryClassification::TypeConflictDirOverFile;
        const bool isCaseCollision =
            m_entry.classification == LocalMergeEntryClassification::CaseInsensitiveCollision;

        const bool overwritePermitted = isExactRegularFileConflict && m_capabilities.canSafeOverwrite;
        const bool renamePermitted =
            m_capabilities.canRenameConflictingCopies
            && m_entry.sourceKind == LocalMergeObjectKind::RegularFile
            && (m_entry.classification == LocalMergeEntryClassification::FileConflict
                || m_entry.classification == LocalMergeEntryClassification::CaseInsensitiveCollision);
        const bool applyToAllEligible = isExactRegularFileConflict && m_remainingExactConflicts > 0;

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);

        // Compact header + discreet conflict counter
        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(8);
        auto *iconLabel = new QLabel(this);
        iconLabel->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(28, 28));
        headerRow->addWidget(iconLabel, 0, Qt::AlignTop);

        auto *titleBox = new QVBoxLayout();
        titleBox->setSpacing(2);
        m_titleLabel = new QLabel(
            trText("Plik „%1” już istnieje", "File “%1” already exists").arg(fileName),
            this);
        QFont titleFont = m_titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(titleFont.pointSizeF() + 1.0);
        m_titleLabel->setFont(titleFont);
        m_titleLabel->setWordWrap(true);
        titleBox->addWidget(m_titleLabel);

        const QString progressText = (progress.totalConflicts > 0 && progress.currentIndex > 0)
            ? trText("Konflikt %1 z %2  •  %3", "Conflict %1 of %2  •  %3")
                  .arg(progress.currentIndex)
                  .arg(progress.totalConflicts)
                  .arg(m_entry.relativePath)
            : m_entry.relativePath;
        m_progressLabel = new QLabel(progressText, this);
        m_progressLabel->setWordWrap(true);
        titleBox->addWidget(m_progressLabel);
        headerRow->addLayout(titleBox, 1);
        root->addLayout(headerRow);

        // Concise conflict type + honest Dolphin-inspired comparison note
        QString conflictKindText;
        if (isExactRegularFileConflict) {
            const bool diffSize = m_entry.sourceStatValid && m_entry.destinationStatValid
                && m_entry.size != m_entry.destinationSize;
            conflictKindText = diffSize
                ? trText("Dwa zwykłe pliki o tej samej nazwie (różny rozmiar).",
                         "Two regular files with the same name (different sizes).")
                : trText("Dwa zwykłe pliki o tej samej nazwie (zawartość nieporównana).",
                         "Two regular files with the same name (content not compared).");
        } else if (isSymlinkConflict) {
            conflictKindText = trText("Konflikt z dowiązaniem symbolicznym (symlink) — zastępowanie zablokowane.",
                                      "Symbolic link conflict — overwriting is blocked.");
        } else if (isTypeConflict) {
            conflictKindText = trText("Niezgodność typów (plik vs folder) — zastępowanie zablokowane.",
                                      "Object type mismatch (file vs folder) — overwriting is blocked.");
        } else if (isCaseCollision) {
            conflictKindText = trText("Kolizja wielkości liter — zastępowanie zablokowane.",
                                      "Case-insensitive name collision — overwriting is blocked.");
        } else {
            conflictKindText = trText("Niejednoznaczny lub zablokowany obiekt — zastępowanie zablokowane.",
                                      "Ambiguous or blocked object — overwriting is blocked.");
        }
        m_conflictTypeLabel = new QLabel(conflictKindText, this);
        m_conflictTypeLabel->setWordWrap(true);
        root->addWidget(m_conflictTypeLabel);

        // Two-column Dolphin-inspired Source / Destination comparison
        auto *cmpRow = new QHBoxLayout();
        cmpRow->setSpacing(8);
        const QIcon srcIcon = LocalMergeDialogDetail::iconForMergeEntry(m_entry.sourceKind, fileName, style());
        const QIcon dstIcon = LocalMergeDialogDetail::iconForMergeEntry(m_entry.destinationKind, fileName, style());

        cmpRow->addWidget(LocalMergeDialogDetail::buildComparisonCard(
            trText("Element źródłowy", "Source item"),
            srcIcon, fileName,
            formatFileSize(static_cast<qint64>(m_entry.size), m_entry.sourceKind == LocalMergeObjectKind::Directory),
            LocalMergeDialogDetail::formatTimestampFromStat(m_entry.sourceStat, m_entry.sourceStatValid, m_locale),
            m_entry.sourcePath, &m_sourcePathLabel, this), 1);

        cmpRow->addWidget(LocalMergeDialogDetail::buildComparisonCard(
            trText("Element docelowy", "Destination item"),
            dstIcon, fileName,
            formatFileSize(static_cast<qint64>(m_entry.destinationSize), m_entry.destinationKind == LocalMergeObjectKind::Directory),
            LocalMergeDialogDetail::formatTimestampFromStat(m_entry.destinationStat, m_entry.destinationStatValid, m_locale),
            m_entry.destinationPath, &m_destPathLabel, this), 1);
        root->addLayout(cmpRow);

        // Essential safety reason (concise in main view; full reason in tooltip & Details)
        QString safetyText;
        QString fullSafetyText;
        if (isExactRegularFileConflict && !m_capabilities.canSafeOverwrite) {
            safetyText = trText(
                "Zastępowanie niedostępne na tym systemie plików. Możesz pominąć plik lub zmienić nazwę kopii.",
                "Overwriting unavailable on this filesystem. You can skip the file or rename the copy.");
            fullSafetyText = m_capabilities.overwriteUnavailableReason.isEmpty()
                ? trText("Zastępowanie plików na tym systemie plików jest obecnie niedostępne ze względów bezpieczeństwa (brak zweryfikowanej atomowej wymiany/trwałości). Możesz pominąć konflikt lub zapisać kopię pod inną nazwą.",
                         "Overwriting files on this filesystem is currently unavailable for safety reasons (atomic exchange/durability unverified). You can skip the conflict or save a copy under another name.")
                : m_capabilities.overwriteUnavailableReason;
        } else if (!isExactRegularFileConflict) {
            safetyText = trText("Zastępowanie zablokowane dla symlinków, konfliktów typów i niejednoznacznych nazw.",
                                "Overwriting blocked for symlinks, type conflicts, and ambiguous names.");
            fullSafetyText = safetyText;
        }
        m_safetyLabel = new QLabel(safetyText, this);
        m_safetyLabel->setWordWrap(true);
        m_safetyLabel->setVisible(!safetyText.isEmpty());
        root->addWidget(m_safetyLabel);

        // Understated, always-visible Rename row (not auto-focused)
        auto *renameBox = new QVBoxLayout();
        renameBox->setSpacing(2);
        auto *renameRow = new QHBoxLayout();
        renameRow->setSpacing(6);
        auto *renamePrompt = new QLabel(trText("Nowa nazwa:", "New name:"), this);
        m_renameEdit = new QLineEdit(this);
        m_renameEdit->setText(LocalMergeDialogDetail::suggestRenamedFile(fileName, m_existingSiblings, m_polish));
        m_renameEdit->setEnabled(renamePermitted);
        m_suggestButton = new QPushButton(trText("Zaproponuj nazwę", "Suggest name"), this);
        m_suggestButton->setAutoDefault(false);
        m_suggestButton->setEnabled(renamePermitted);
        connect(m_suggestButton, &QPushButton::clicked, this, [this, fileName] {
            m_renameEdit->setText(LocalMergeDialogDetail::suggestRenamedFile(fileName, m_existingSiblings, m_polish));
        });
        renameRow->addWidget(renamePrompt);
        renameRow->addWidget(m_renameEdit, 1);
        renameRow->addWidget(m_suggestButton);
        renameBox->addLayout(renameRow);

        m_validationLabel = new QLabel(this);
        m_validationLabel->setWordWrap(true);
        m_validationLabel->setVisible(false);
        renameBox->addWidget(m_validationLabel);
        root->addLayout(renameBox);

        // Apply to all checkbox (strictly scoped)
        m_applyToAllCheck = new QCheckBox(
            trText("Zastosuj do wszystkich pozostałych konfliktów plików w tym zadaniu",
                   "Apply to all remaining file conflicts in this task"),
            this);
        m_applyToAllCheck->setEnabled(applyToAllEligible);
        m_applyToAllCheck->setVisible(applyToAllEligible);
        root->addWidget(m_applyToAllCheck);

        // Collapsible Details section for backend capabilities & full paths
        m_detailsToggle = new QToolButton(this);
        m_detailsToggle->setText(trText("Szczegóły ▸", "Details ▸"));
        m_detailsToggle->setCheckable(true);
        m_detailsToggle->setAutoRaise(true);
        root->addWidget(m_detailsToggle, 0, Qt::AlignLeft);

        m_detailsFrame = new QFrame(this);
        m_detailsFrame->setFrameShape(QFrame::StyledPanel);
        m_detailsFrame->setVisible(false);
        auto *detLay = new QVBoxLayout(m_detailsFrame);
        detLay->setContentsMargins(8, 6, 8, 6);
        detLay->setSpacing(4);
        const QString capSummary = trText(
            "Możliwości backendu (%1): scalanie folderów: %2 • nowe pliki: %3 • zmiana nazwy kopii: %4 • bezpieczne zastępowanie: %5",
            "Backend capabilities (%1): folder merge: %2 • new files: %3 • copy rename: %4 • safe overwrite: %5")
            .arg(m_capabilities.filesystemLabel.isEmpty() ? trText("lokalny", "local") : m_capabilities.filesystemLabel)
            .arg(m_capabilities.canMergeDirectories ? trText("tak", "yes") : trText("nie", "no"))
            .arg(m_capabilities.canCreateNewFiles ? trText("tak", "yes") : trText("nie", "no"))
            .arg(m_capabilities.canRenameConflictingCopies ? trText("tak", "yes") : trText("nie", "no"))
            .arg(m_capabilities.canSafeOverwrite ? trText("tak", "yes") : trText("nie", "no"));
        m_capabilitiesLabel = new QLabel(capSummary, m_detailsFrame);
        m_capabilitiesLabel->setWordWrap(true);
        detLay->addWidget(m_capabilitiesLabel);

        if (!fullSafetyText.isEmpty() && fullSafetyText != safetyText) {
            auto *detSafety = new QLabel(fullSafetyText, m_detailsFrame);
            detSafety->setWordWrap(true);
            detLay->addWidget(detSafety);
        }

        detLay->addWidget(LocalMergeDialogDetail::makeDetailsPathsView(
            trText("Pełne źródło: %1\nPełny cel: %2", "Full source: %1\nFull destination: %2")
                .arg(m_entry.sourcePath, m_entry.destinationPath),
            m_detailsFrame));
        root->addWidget(m_detailsFrame);

        connect(m_detailsToggle, &QToolButton::toggled, this, [this](bool on) {
            m_detailsToggle->setText(on ? trText("Szczegóły ▾", "Details ▾") : trText("Szczegóły ▸", "Details ▸"));
            LocalMergeDialogDetail::applyDetailsToggleGeometry(
                this, m_detailsFrame, on, m_savedCompactSize, m_userResized, m_programmaticResize);
        });

        // Compact 4-decision bar (Skip is ALWAYS the safe default; Overwrite NEVER default/autoDefault)
        auto *btnRow = new QHBoxLayout();
        btnRow->setSpacing(6);
        btnRow->addStretch(1);

        m_overwriteButton = new QPushButton(trText("Zastąp", "Overwrite"), this);
        m_overwriteButton->setAutoDefault(false);
        m_overwriteButton->setDefault(false);
        m_overwriteButton->setEnabled(overwritePermitted);
        if (!overwritePermitted && !fullSafetyText.isEmpty()) {
            m_overwriteButton->setToolTip(fullSafetyText);
        }
        connect(m_overwriteButton, &QPushButton::clicked, this, [this] {
            m_decision.action = LocalMergeApprovedAction::Overwrite;
            m_decision.applyPolicy = m_applyToAllCheck->isChecked()
                ? LocalMergeApplyPolicy::OverwriteAllFileConflicts
                : LocalMergeApplyPolicy::None;
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_overwriteButton);

        m_renameButton = new QPushButton(trText("Zmień nazwę", "Rename"), this);
        m_renameButton->setAutoDefault(false);
        m_renameButton->setEnabled(renamePermitted);
        if (!renamePermitted) {
            m_renameButton->setToolTip(trText(
                "Zmiana nazwy jest niedostępna dla tego typu konfliktu.",
                "Renaming is unavailable for this conflict type."));
        }
        connect(m_renameButton, &QPushButton::clicked, this, [this] {
            if (!validateRenameInput()) {
                return;
            }
            m_decision.action = LocalMergeApprovedAction::Rename;
            m_decision.applyPolicy = LocalMergeApplyPolicy::None;
            m_decision.targetFileName = m_renameEdit->text();
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_renameButton);

        m_skipButton = new QPushButton(trText("Pomiń", "Skip"), this);
        m_skipButton->setDefault(true);
        m_skipButton->setAutoDefault(true);
        m_skipButton->setFocus();
        connect(m_skipButton, &QPushButton::clicked, this, [this] {
            m_decision.action = LocalMergeApprovedAction::Skip;
            m_decision.applyPolicy = m_applyToAllCheck->isChecked()
                ? LocalMergeApplyPolicy::SkipAllFileConflicts
                : LocalMergeApplyPolicy::None;
            m_decision.accepted = true;
            accept();
        });
        btnRow->addWidget(m_skipButton);

        m_cancelButton = new QPushButton(trText("Anuluj", "Cancel"), this);
        m_cancelButton->setAutoDefault(false);
        connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        btnRow->addWidget(m_cancelButton);

        root->addLayout(btnRow);

        connect(m_renameEdit, &QLineEdit::textChanged, this, [this] { validateRenameInput(); });
        connect(m_renameEdit, &QLineEdit::returnPressed, this, [this] {
            if (m_renameButton->isEnabled() && validateRenameInput()) {
                m_renameButton->click();
            }
        });
        validateRenameInput();
    }

    LocalFileConflictDecision decision() const { return m_decision; }
    QPushButton *overwriteButton() const { return m_overwriteButton; }
    QPushButton *renameButton() const { return m_renameButton; }
    QPushButton *skipButton() const { return m_skipButton; }
    QPushButton *cancelButton() const { return m_cancelButton; }
    QPushButton *suggestButton() const { return m_suggestButton; }
    QLineEdit *renameLineEdit() const { return m_renameEdit; }
    QCheckBox *applyToAllCheckBox() const { return m_applyToAllCheck; }
    QLabel *progressLabel() const { return m_progressLabel; }
    QLabel *safetyLabel() const { return m_safetyLabel; }
    QLabel *capabilitiesLabel() const { return m_capabilitiesLabel; }
    QLabel *conflictTypeLabel() const { return m_conflictTypeLabel; }
    QLabel *validationLabel() const { return m_validationLabel; }
    LocalMergeDialogDetail::ElidedPathLabel *sourcePathLabel() const { return m_sourcePathLabel; }
    LocalMergeDialogDetail::ElidedPathLabel *destinationPathLabel() const { return m_destPathLabel; }
    QFrame *detailsFrame() const { return m_detailsFrame; }
    QToolButton *detailsToggleButton() const { return m_detailsToggle; }

    void reject() override
    {
        m_decision = LocalFileConflictDecision{LocalMergeApprovedAction::Abort, LocalMergeApplyPolicy::None, {}, false};
        QDialog::reject();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        if (!m_programmaticResize && isVisible() && m_detailsToggle && !m_detailsToggle->isChecked()) {
            m_userResized = true;
        }
    }

private:
    QString trText(const char *pl, const char *en) const
    {
        return m_polish ? QString::fromUtf8(pl) : QString::fromUtf8(en);
    }

    bool validateRenameInput()
    {
        if (!m_renameEdit->isEnabled()) {
            m_renameButton->setEnabled(false);
            return false;
        }
        const QString name = m_renameEdit->text();
        QString err;
        if (!LocalMergeDialogDetail::isSafeSingleComponent(name)) {
            err = trText("Nazwa musi być pojedynczym bezpiecznym elementem (bez /, \\ oraz ..).",
                         "Name must be a single safe component (without /, \\ or ..).");
        } else if (LocalMergeDialogDetail::hasCaseInsensitiveCollision(name, m_existingSiblings)) {
            err = trText("Ta nazwa koliduje z istniejącym elementem w folderze docelowym.",
                         "This name collides with an existing item in the destination folder.");
        }
        const bool ok = err.isEmpty();
        m_renameButton->setEnabled(ok);
        m_validationLabel->setText(err);
        m_validationLabel->setVisible(!ok);
        return ok;
    }

    LocalMergePlanEntry m_entry;
    LocalMergeBackendCapabilities m_capabilities;
    qsizetype m_remainingExactConflicts = 0;
    QSet<QString> m_existingSiblings;
    QLocale m_locale;
    bool m_polish = false;
    LocalFileConflictDecision m_decision;

    QLabel *m_titleLabel = nullptr;
    QLabel *m_conflictTypeLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    LocalMergeDialogDetail::ElidedPathLabel *m_sourcePathLabel = nullptr;
    LocalMergeDialogDetail::ElidedPathLabel *m_destPathLabel = nullptr;
    QLabel *m_capabilitiesLabel = nullptr;
    QLabel *m_safetyLabel = nullptr;
    QToolButton *m_detailsToggle = nullptr;
    QFrame *m_detailsFrame = nullptr;
    QSize m_savedCompactSize;
    bool m_userResized = false;
    bool m_programmaticResize = false;
    QLineEdit *m_renameEdit = nullptr;
    QPushButton *m_suggestButton = nullptr;
    QLabel *m_validationLabel = nullptr;
    QCheckBox *m_applyToAllCheck = nullptr;
    QPushButton *m_overwriteButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_skipButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};

struct LocalMergeResolutionResult
{
    bool completed = false;
    bool cancelled = false;
    qsizetype folderPromptsShown = 0;
    qsizetype filePromptsShown = 0;
    qsizetype nonConflictingAutoApproved = 0;
    qsizetype subtreesSkipped = 0;
    qsizetype renamedSubtreeEntriesRemapped = 0;
    QStringList entriesRequiringStage3BRevalidation;
};

// Pure-plan, zero-I/O resumable conflict resolution session for Stage 3A.
// Caller must keep `plan` alive for the lifetime of the session.
class LocalMergeConflictSession
{
public:
    enum class State {
        Ready,
        WaitingFolderDecision,
        WaitingFileDecision,
        Completed,
        Cancelled
    };

    enum class StepKind {
        NeedFolderDecision,
        NeedFileDecision,
        Completed,
        Cancelled
    };

    struct Step {
        StepKind kind = StepKind::Completed;
        LocalMergePlanEntry entry;
        qsizetype remainingConflicts = 0;
        LocalMergeConflictProgress progress;
    };

    explicit LocalMergeConflictSession(
        LocalMergePlan *plan,
        LocalMergeBackendCapabilities capabilities = {})
        : m_plan(plan)
        , m_capabilities(capabilities)
    {
        if (!m_plan) {
            m_state = State::Cancelled;
            m_result.cancelled = true;
            m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
        }
    }

    State state() const { return m_state; }
    bool isTerminal() const { return m_state == State::Completed || m_state == State::Cancelled; }
    const LocalMergeResolutionResult &result() const { return m_result; }

    Step advance()
    {
        if (m_state == State::Completed) {
            return Step{StepKind::Completed, {}, 0, {}};
        }
        if (m_state == State::Cancelled || !m_plan) {
            return Step{StepKind::Cancelled, {}, 0, {}};
        }
        if (m_state == State::WaitingFolderDecision || m_state == State::WaitingFileDecision) {
            return m_pendingStep;
        }

        while (m_index < m_plan->entries.size()) {
            const qsizetype i = m_index;
            LocalMergePlanEntry &entry = m_plan->entries[i];

            if (hasSkippedAncestor(entry.relativePath)) {
                entry.approvedAction = LocalMergeApprovedAction::Skip;
                entry.decisionBinding.reason = QStringLiteral("Parent folder skipped");
                ++m_index;
                continue;
            }

            if (const RenamedSubtree *rs = findRenamedAncestor(entry.relativePath)) {
                const QString tail = entry.relativePath.mid(rs->oldRelPrefix.size() + 1);
                entry.relativePath = rs->newRelPrefix + QLatin1Char('/') + tail;
                entry.destinationPath = QDir::cleanPath(rs->newDestPrefix + QLatin1Char('/') + tail);
                entry.classification = LocalMergeEntryClassification::CreateNew;
                entry.approvedAction = LocalMergeApprovedAction::Create;
                entry.decisionBinding.reason = QStringLiteral("Remapped under renamed parent folder (requires Stage 3B revalidation)");
                ++m_result.renamedSubtreeEntriesRemapped;
                m_result.entriesRequiringStage3BRevalidation.append(entry.relativePath);
                ++m_index;
                continue;
            }

            if (entry.classification == LocalMergeEntryClassification::CreateNew
                && (entry.sourceKind == LocalMergeObjectKind::Directory
                    || entry.sourceKind == LocalMergeObjectKind::RegularFile
                    || (entry.sourceKind == LocalMergeObjectKind::SymbolicLink
                        && !entry.symbolicLinkContents.isEmpty()))) {
                entry.approvedAction = LocalMergeApprovedAction::Create;
                ++m_result.nonConflictingAutoApproved;
                ++m_index;
                continue;
            }
            if (isAutoApprovableSourceSymlink(entry)) {
                entry.approvedAction = LocalMergeApprovedAction::Create;
                ++m_result.nonConflictingAutoApproved;
                ++m_index;
                continue;
            }

            if (isFolderPromptCandidate(entry)) {
                const bool exactMerge =
                    entry.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
                    && entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch;
                if (exactMerge && m_folderPolicy == LocalMergeApplyPolicy::MergeAllFolders) {
                    entry.approvedAction = LocalMergeApprovedAction::MergeDirectory;
                    entry.decisionBinding.policyScope = LocalMergeApplyPolicy::MergeAllFolders;
                    ++m_index;
                    continue;
                }
                if (exactMerge && m_folderPolicy == LocalMergeApplyPolicy::SkipAllFolders) {
                    entry.approvedAction = LocalMergeApprovedAction::Skip;
                    entry.decisionBinding.policyScope = LocalMergeApplyPolicy::SkipAllFolders;
                    m_skippedFolderPrefixes.insert(entry.relativePath);
                    ++m_result.subtreesSkipped;
                    ++m_index;
                    continue;
                }

                ++m_currentConflictIdx;
                ++m_result.folderPromptsShown;
                const qsizetype dynamicTotal = (m_currentConflictIdx - 1) + countRemainingPromptableFrom(i);
                m_pendingEntryIndex = i;
                m_pendingExact = exactMerge;
                m_state = State::WaitingFolderDecision;
                m_pendingStep = Step{
                    StepKind::NeedFolderDecision,
                    entry,
                    countRemaining(i, true),
                    LocalMergeConflictProgress{m_currentConflictIdx, dynamicTotal}
                };
                return m_pendingStep;
            }

            if (isFilePromptCandidate(entry)) {
                const bool exactRegular =
                    entry.classification == LocalMergeEntryClassification::FileConflict
                    && entry.sourceKind == LocalMergeObjectKind::RegularFile
                    && entry.destinationKind == LocalMergeObjectKind::RegularFile
                    && entry.caseStatus == LocalMergeCaseStatus::ExactNameMatch;
                if (exactRegular && m_filePolicy == LocalMergeApplyPolicy::SkipAllFileConflicts) {
                    entry.approvedAction = LocalMergeApprovedAction::Skip;
                    entry.decisionBinding.policyScope = LocalMergeApplyPolicy::SkipAllFileConflicts;
                    ++m_index;
                    continue;
                }
                if (exactRegular && m_capabilities.canSafeOverwrite
                    && m_filePolicy == LocalMergeApplyPolicy::OverwriteAllFileConflicts) {
                    entry.approvedAction = LocalMergeApprovedAction::Overwrite;
                    entry.decisionBinding.policyScope = LocalMergeApplyPolicy::OverwriteAllFileConflicts;
                    ++m_index;
                    continue;
                }

                ++m_currentConflictIdx;
                ++m_result.filePromptsShown;
                const qsizetype dynamicTotal = (m_currentConflictIdx - 1) + countRemainingPromptableFrom(i);
                m_pendingEntryIndex = i;
                m_pendingExact = exactRegular;
                m_state = State::WaitingFileDecision;
                m_pendingStep = Step{
                    StepKind::NeedFileDecision,
                    entry,
                    countRemaining(i, false),
                    LocalMergeConflictProgress{m_currentConflictIdx, dynamicTotal}
                };
                return m_pendingStep;
            }

            ++m_index;
        }

        m_state = State::Completed;
        m_result.completed = true;
        m_pendingEntryIndex = -1;
        m_pendingExact = false;
        m_pendingStep = Step{StepKind::Completed, {}, 0, {}};
        return m_pendingStep;
    }

    bool submitFolderDecision(const LocalFolderConflictDecision &dec)
    {
        if (m_state != State::WaitingFolderDecision || !m_plan
            || m_pendingEntryIndex < 0 || m_pendingEntryIndex >= m_plan->entries.size()) {
            return false;
        }

        LocalMergePlanEntry &entry = m_plan->entries[m_pendingEntryIndex];
        if (!dec.accepted || dec.action == LocalMergeApprovedAction::Abort) {
            entry.approvedAction = LocalMergeApprovedAction::Abort;
            m_result.cancelled = true;
            m_state = State::Cancelled;
            m_pendingEntryIndex = -1;
            m_pendingExact = false;
            m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
            return true;
        }

        // SEC-046-09: Fail closed if Rename or MergeDirectory is submitted for a SameObject
        // directory (planner skips descendants of SameObject, so Rename would silently create
        // an empty directory and report Completed). Only Skip or Abort is permitted.
        if (entry.classification == LocalMergeEntryClassification::SameObject
            && (dec.action == LocalMergeApprovedAction::Rename
                || dec.action == LocalMergeApprovedAction::MergeDirectory)) {
            entry.approvedAction = LocalMergeApprovedAction::Abort;
            m_result.cancelled = true;
            m_state = State::Cancelled;
            m_pendingEntryIndex = -1;
            m_pendingExact = false;
            m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
            return false;
        }

        if (dec.action == LocalMergeApprovedAction::MergeDirectory) {
            entry.approvedAction = LocalMergeApprovedAction::MergeDirectory;
            if (m_pendingExact && dec.applyPolicy == LocalMergeApplyPolicy::MergeAllFolders) {
                m_folderPolicy = LocalMergeApplyPolicy::MergeAllFolders;
                entry.decisionBinding.policyScope = LocalMergeApplyPolicy::MergeAllFolders;
            }
        } else if (dec.action == LocalMergeApprovedAction::Skip) {
            entry.approvedAction = LocalMergeApprovedAction::Skip;
            m_skippedFolderPrefixes.insert(entry.relativePath);
            ++m_result.subtreesSkipped;
            if (m_pendingExact && dec.applyPolicy == LocalMergeApplyPolicy::SkipAllFolders) {
                m_folderPolicy = LocalMergeApplyPolicy::SkipAllFolders;
                entry.decisionBinding.policyScope = LocalMergeApplyPolicy::SkipAllFolders;
            }
        } else if (dec.action == LocalMergeApprovedAction::Rename) {
            const QString oldRel = entry.relativePath;
            const qsizetype slash = oldRel.lastIndexOf(QLatin1Char('/'));
            const QString newRel = slash > 0
                ? (oldRel.left(slash + 1) + dec.targetFolderName)
                : dec.targetFolderName;
            const QString newDest = QDir::cleanPath(
                QDir(QFileInfo(entry.destinationPath).path()).filePath(dec.targetFolderName));
            entry.approvedAction = LocalMergeApprovedAction::Rename;
            entry.decisionBinding.targetFileName = dec.targetFolderName;
            entry.relativePath = newRel;
            entry.destinationPath = newDest;
            m_renamedSubtrees.append(RenamedSubtree{oldRel, newRel, newDest});
            m_result.entriesRequiringStage3BRevalidation.append(newRel);
        }

        m_index = m_pendingEntryIndex + 1;
        m_pendingEntryIndex = -1;
        m_pendingExact = false;
        m_pendingStep = Step{};
        m_state = State::Ready;
        return true;
    }

    bool submitFileDecision(const LocalFileConflictDecision &dec)
    {
        if (m_state != State::WaitingFileDecision || !m_plan
            || m_pendingEntryIndex < 0 || m_pendingEntryIndex >= m_plan->entries.size()) {
            return false;
        }

        LocalMergePlanEntry &entry = m_plan->entries[m_pendingEntryIndex];
        if (!dec.accepted || dec.action == LocalMergeApprovedAction::Abort) {
            entry.approvedAction = LocalMergeApprovedAction::Abort;
            m_result.cancelled = true;
            m_state = State::Cancelled;
            m_pendingEntryIndex = -1;
            m_pendingExact = false;
            m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
            return true;
        }

        // SEC-046-09: Fail closed if Rename or MergeDirectory is submitted for a SameObject
        // directory (planner skips descendants of SameObject, so Rename would silently create
        // an empty directory and report Completed). Only Skip or Abort is permitted.
        if (entry.classification == LocalMergeEntryClassification::SameObject
            && entry.sourceKind == LocalMergeObjectKind::Directory
            && (dec.action == LocalMergeApprovedAction::Rename
                || dec.action == LocalMergeApprovedAction::MergeDirectory)) {
            entry.approvedAction = LocalMergeApprovedAction::Abort;
            m_result.cancelled = true;
            m_state = State::Cancelled;
            m_pendingEntryIndex = -1;
            m_pendingExact = false;
            m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
            return false;
        }

        entry.approvedAction = dec.action;
        if (dec.action == LocalMergeApprovedAction::Skip
            && entry.sourceKind == LocalMergeObjectKind::Directory) {
            m_skippedFolderPrefixes.insert(entry.relativePath);
            ++m_result.subtreesSkipped;
        } else if (dec.action == LocalMergeApprovedAction::Rename) {
            entry.decisionBinding.targetFileName = dec.targetFileName;
        } else if (m_pendingExact && dec.action == LocalMergeApprovedAction::Skip
                   && dec.applyPolicy == LocalMergeApplyPolicy::SkipAllFileConflicts) {
            m_filePolicy = LocalMergeApplyPolicy::SkipAllFileConflicts;
            entry.decisionBinding.policyScope = LocalMergeApplyPolicy::SkipAllFileConflicts;
        } else if (m_pendingExact && m_capabilities.canSafeOverwrite
                   && dec.action == LocalMergeApprovedAction::Overwrite
                   && dec.applyPolicy == LocalMergeApplyPolicy::OverwriteAllFileConflicts) {
            m_filePolicy = LocalMergeApplyPolicy::OverwriteAllFileConflicts;
            entry.decisionBinding.policyScope = LocalMergeApplyPolicy::OverwriteAllFileConflicts;
        }

        m_index = m_pendingEntryIndex + 1;
        m_pendingEntryIndex = -1;
        m_pendingExact = false;
        m_pendingStep = Step{};
        m_state = State::Ready;
        return true;
    }

    void cancel()
    {
        if (isTerminal()) {
            return;
        }
        if ((m_state == State::WaitingFolderDecision || m_state == State::WaitingFileDecision)
            && m_plan && m_pendingEntryIndex >= 0 && m_pendingEntryIndex < m_plan->entries.size()) {
            m_plan->entries[m_pendingEntryIndex].approvedAction = LocalMergeApprovedAction::Abort;
        }
        m_result.cancelled = true;
        m_state = State::Cancelled;
        m_pendingEntryIndex = -1;
        m_pendingExact = false;
        m_pendingStep = Step{StepKind::Cancelled, {}, 0, {}};
    }

private:
    struct RenamedSubtree {
        QString oldRelPrefix;
        QString newRelPrefix;
        QString newDestPrefix;
    };

    bool hasSkippedAncestor(const QString &rel) const
    {
        for (const QString &pref : m_skippedFolderPrefixes) {
            if (rel == pref || rel.startsWith(pref + QLatin1Char('/'))) {
                return true;
            }
        }
        return false;
    }

    qsizetype countRemainingPromptableFrom(qsizetype fromIndex) const
    {
        qsizetype rem = 0;
        for (qsizetype j = fromIndex; j < m_plan->entries.size(); ++j) {
            const auto &other = m_plan->entries.at(j);
            if (hasSkippedAncestor(other.relativePath)) continue;
            if (isFolderPromptCandidate(other) || isFilePromptCandidate(other)) {
                ++rem;
            }
        }
        return rem;
    }

    const RenamedSubtree *findRenamedAncestor(const QString &rel) const
    {
        const RenamedSubtree *best = nullptr;
        for (const auto &rs : m_renamedSubtrees) {
            if (rel.startsWith(rs.oldRelPrefix + QLatin1Char('/'))) {
                if (!best || rs.oldRelPrefix.size() > best->oldRelPrefix.size()) {
                    best = &rs;
                }
            }
        }
        return best;
    }

    qsizetype countRemaining(qsizetype fromIndex, bool folderSide) const
    {
        qsizetype rem = 0;
        for (qsizetype j = fromIndex + 1; j < m_plan->entries.size(); ++j) {
            const auto &other = m_plan->entries.at(j);
            if (hasSkippedAncestor(other.relativePath)) continue;
            if (folderSide) {
                if (other.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
                    && other.caseStatus == LocalMergeCaseStatus::ExactNameMatch) {
                    ++rem;
                }
            } else {
                if (other.classification == LocalMergeEntryClassification::FileConflict
                    && other.caseStatus == LocalMergeCaseStatus::ExactNameMatch
                    && other.sourceKind == LocalMergeObjectKind::RegularFile
                    && other.destinationKind == LocalMergeObjectKind::RegularFile) {
                    ++rem;
                }
            }
        }
        return rem;
    }

    static bool isAutoApprovableSourceSymlink(const LocalMergePlanEntry &e)
    {
        return e.classification == LocalMergeEntryClassification::BrokenSymlink
            && e.sourceKind == LocalMergeObjectKind::SymbolicLink
            && e.destinationKind == LocalMergeObjectKind::Missing
            && !e.symbolicLinkContents.isEmpty();
    }

    static bool isFolderPromptCandidate(const LocalMergePlanEntry &e)
    {
        return e.classification == LocalMergeEntryClassification::MergeDirectoryCandidate
            || (e.classification == LocalMergeEntryClassification::SameObject
                && e.sourceKind == LocalMergeObjectKind::Directory);
    }

    static bool isFilePromptCandidate(const LocalMergePlanEntry &e)
    {
        if (isAutoApprovableSourceSymlink(e)) {
            return false;
        }
        return e.classification == LocalMergeEntryClassification::FileConflict
            || e.classification == LocalMergeEntryClassification::DestinationSymlinkConflict
            || e.classification == LocalMergeEntryClassification::BrokenSymlink
            || (e.classification == LocalMergeEntryClassification::SameObject
                && e.sourceKind != LocalMergeObjectKind::Directory)
            || e.classification == LocalMergeEntryClassification::TypeConflictFileOverDir
            || e.classification == LocalMergeEntryClassification::TypeConflictDirOverFile
            || e.classification == LocalMergeEntryClassification::CaseInsensitiveCollision;
    }

    LocalMergePlan *m_plan = nullptr;
    LocalMergeBackendCapabilities m_capabilities;
    LocalMergeResolutionResult m_result;
    LocalMergeApplyPolicy m_folderPolicy = LocalMergeApplyPolicy::None;
    LocalMergeApplyPolicy m_filePolicy = LocalMergeApplyPolicy::None;
    QSet<QString> m_skippedFolderPrefixes;
    QList<RenamedSubtree> m_renamedSubtrees;
    qsizetype m_currentConflictIdx = 0;
    qsizetype m_index = 0;
    qsizetype m_pendingEntryIndex = -1;
    bool m_pendingExact = false;
    State m_state = State::Ready;
    Step m_pendingStep;
};

// Pure-plan, zero-I/O recursive conflict resolution coordinator for Stage 3A.
class LocalMergeConflictResolver
{
public:
    using FolderPromptFn = std::function<LocalFolderConflictDecision(
        const LocalMergePlanEntry &entry,
        qsizetype remainingFolderConflicts,
        LocalMergeConflictProgress progress)>;
    using FilePromptFn = std::function<LocalFileConflictDecision(
        const LocalMergePlanEntry &entry,
        const LocalMergeBackendCapabilities &capabilities,
        qsizetype remainingExactFileConflicts,
        LocalMergeConflictProgress progress)>;

    static LocalMergeResolutionResult resolve(
        LocalMergePlan &plan,
        const LocalMergeBackendCapabilities &capabilities,
        const FolderPromptFn &folderPrompt,
        const FilePromptFn &filePrompt)
    {
        LocalMergeConflictSession session(&plan, capabilities);
        while (!session.isTerminal()) {
            const LocalMergeConflictSession::Step step = session.advance();
            if (step.kind == LocalMergeConflictSession::StepKind::NeedFolderDecision) {
                const LocalFolderConflictDecision dec = folderPrompt
                    ? folderPrompt(step.entry, step.remainingConflicts, step.progress)
                    : LocalFolderConflictDecision{};
                if (!session.submitFolderDecision(dec)) {
                    session.cancel();
                    break;
                }
            } else if (step.kind == LocalMergeConflictSession::StepKind::NeedFileDecision) {
                const LocalFileConflictDecision dec = filePrompt
                    ? filePrompt(step.entry, capabilities, step.remainingConflicts, step.progress)
                    : LocalFileConflictDecision{};
                if (!session.submitFileDecision(dec)) {
                    session.cancel();
                    break;
                }
            } else {
                break;
            }
        }
        return session.result();
    }
};
