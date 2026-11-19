/*
 * Interactive Manual GUI Preview Launcher for FOLDER-MERGE-01 Stage 3A (UX Polish).
 * Uses the exact production dialog classes (LocalFolderMergeConflictDialog,
 * LocalFileConflictResolutionDialog, LocalMergeConflictResolver) on synthetic
 * demonstration plans with ZERO filesystem I/O.
 *
 * SPDX-FileCopyrightText: 2026 Sebastian Harasim
 * SPDX-License-Identifier: MIT
 */

#include "localmergedialogs.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {

LocalMergePlanEntry makeFolder(const QString &rel, const QString &src, const QString &dst, bool selfCopy = false)
{
    LocalMergePlanEntry e;
    e.relativePath = rel;
    e.sourcePath = src;
    e.destinationPath = dst;
    e.sourceKind = LocalMergeObjectKind::Directory;
    e.destinationKind = LocalMergeObjectKind::Directory;
    e.classification = selfCopy ? LocalMergeEntryClassification::SameObject
                                : LocalMergeEntryClassification::MergeDirectoryCandidate;
    e.caseStatus = LocalMergeCaseStatus::ExactNameMatch;
    e.pendingDecision = LocalMergePendingDecision::AwaitUserFolderDecision;
    return e;
}

LocalMergePlanEntry makeFile(const QString &rel, const QString &src, const QString &dst, qulonglong srcSize = 4096, qulonglong dstSize = 2048)
{
    LocalMergePlanEntry e;
    e.relativePath = rel;
    e.sourcePath = src;
    e.destinationPath = dst;
    e.sourceKind = LocalMergeObjectKind::RegularFile;
    e.destinationKind = LocalMergeObjectKind::RegularFile;
    e.classification = LocalMergeEntryClassification::FileConflict;
    e.caseStatus = LocalMergeCaseStatus::ExactNameMatch;
    e.pendingDecision = LocalMergePendingDecision::AwaitUserFileDecision;
    e.size = srcSize;
    e.destinationSize = dstSize;
    e.sourceStatValid = true;
    e.destinationStatValid = true;
    e.sourceStat.st_mtim.tv_sec = 1760000000;
    e.destinationStat.st_mtim.tv_sec = 1759000000;
    return e;
}

LocalMergePlanEntry makeNewFile(const QString &rel, const QString &src, const QString &dst)
{
    LocalMergePlanEntry e;
    e.relativePath = rel;
    e.sourcePath = src;
    e.destinationPath = dst;
    e.sourceKind = LocalMergeObjectKind::RegularFile;
    e.destinationKind = LocalMergeObjectKind::Missing;
    e.classification = LocalMergeEntryClassification::CreateNew;
    e.size = 1024;
    return e;
}

struct DemoScenario
{
    QString id;
    QString titlePl;
    QString titleEn;
    QString hintPl;
    QString hintEn;
    LocalMergePlan plan;
    LocalMergeBackendCapabilities capabilities;
};

DemoScenario buildScenario(int idx)
{
    DemoScenario s;
    s.capabilities.canMergeDirectories = true;
    s.capabilities.canCreateNewFiles = true;
    s.capabilities.canRenameConflictingCopies = true;
    s.capabilities.canSafeOverwrite = true;
    s.capabilities.filesystemLabel = QStringLiteral("Btrfs");

    switch (idx) {
    case 0:
        s.id = QStringLiteral("A");
        s.titlePl = QStringLiteral("A. Konflikt folderu głównego (wszystkie 4 opcje)");
        s.titleEn = QStringLiteral("A. Root folder conflict (all 4 options)");
        s.hintPl = QStringLiteral("Sprawdź Scal / Zmień nazwę / Pomiń / Anuluj dla katalogu głównego DemoProject.");
        s.hintEn = QStringLiteral("Check Merge / Rename / Skip / Cancel for root folder DemoProject.");
        s.plan.entries = {
            makeFolder(QStringLiteral("DemoProject"), QStringLiteral("/demo/source/DemoProject"), QStringLiteral("/demo/target/DemoProject")),
            makeNewFile(QStringLiteral("DemoProject/README.txt"), QStringLiteral("/demo/source/DemoProject/README.txt"), QStringLiteral("/demo/target/DemoProject/README.txt")),
        };
        break;
    case 1:
    case 3:
    case 4:
    case 5:
        s.id = (idx == 1 ? QStringLiteral("B") : idx == 3 ? QStringLiteral("D") : idx == 4 ? QStringLiteral("E") : QStringLiteral("F"));
        if (idx == 1) {
            s.titlePl = QStringLiteral("B. Konflikt podfolderu na 2. i 3. poziomie (rekurencyjnie)");
            s.titleEn = QStringLiteral("B. Subfolder conflicts at levels 2 & 3 (recursive)");
            s.hintPl = QStringLiteral("Scal DemoData → rozstrzygnij Profiles → Profiles/Default → Settings.cfg.");
            s.hintEn = QStringLiteral("Merge DemoData → resolve Profiles → Profiles/Default → Settings.cfg.");
        } else if (idx == 3) {
            s.titlePl = QStringLiteral("D. Pominięcie podfolderu (brak pytań o potomków)");
            s.titleEn = QStringLiteral("D. Skip subfolder (no prompts for descendants)");
            s.hintPl = QStringLiteral("Scal DemoData, a następnie kliknij „Pomiń folder” na DemoData/Profiles — potomkowie znikną z licznika.");
            s.hintEn = QStringLiteral("Merge DemoData, then click Skip folder on DemoData/Profiles — descendants are skipped.");
        } else if (idx == 4) {
            s.titlePl = QStringLiteral("E. Zmiana nazwy podfolderu (przemapowanie potomków + rewalidacja Stage 3B)");
            s.titleEn = QStringLiteral("E. Rename subfolder (remapped descendants + Stage 3B revalidation)");
            s.hintPl = QStringLiteral("Scal DemoData, a dla DemoData/Profiles wybierz „Zmień nazwę”.");
            s.hintEn = QStringLiteral("Merge DemoData, then choose Rename on DemoData/Profiles.");
        } else {
            s.titlePl = QStringLiteral("F. Scal wszystkie foldery (konflikty plików nadal pytają osobno)");
            s.titleEn = QStringLiteral("F. Merge all folders (file conflicts still prompt separately)");
            s.hintPl = QStringLiteral("Zaznacz „Zastosuj do wszystkich pozostałych folderów” przy DemoData i kliknij Scal — pliki nadal zapytają.");
            s.hintEn = QStringLiteral("Check Apply to all remaining folders on DemoData and click Merge — files still prompt.");
        }
        s.plan.entries = {
            makeFolder(QStringLiteral("DemoData"), QStringLiteral("/demo/source/DemoData"), QStringLiteral("/demo/target/DemoData")),
            makeNewFile(QStringLiteral("DemoData/Notes.txt"), QStringLiteral("/demo/source/DemoData/Notes.txt"), QStringLiteral("/demo/target/DemoData/Notes.txt")),
            makeFolder(QStringLiteral("DemoData/Profiles"), QStringLiteral("/demo/source/DemoData/Profiles"), QStringLiteral("/demo/target/DemoData/Profiles")),
            makeFolder(QStringLiteral("DemoData/Profiles/Default"), QStringLiteral("/demo/source/DemoData/Profiles/Default"), QStringLiteral("/demo/target/DemoData/Profiles/Default")),
            makeFile(QStringLiteral("DemoData/Profiles/Default/Settings.cfg"), QStringLiteral("/demo/source/DemoData/Profiles/Default/Settings.cfg"), QStringLiteral("/demo/target/DemoData/Profiles/Default/Settings.cfg")),
            makeFile(QStringLiteral("DemoData/Profiles/Default/Layout.json"), QStringLiteral("/demo/source/DemoData/Profiles/Default/Layout.json"), QStringLiteral("/demo/target/DemoData/Profiles/Default/Layout.json")),
        };
        break;
    case 2:
        s.id = QStringLiteral("C");
        s.titlePl = QStringLiteral("C. Konflikt zwykłego pliku (Zastąp / Zmień nazwę / Pomiń / Anuluj)");
        s.titleEn = QStringLiteral("C. Regular file conflict (Overwrite / Rename / Skip / Cancel)");
        s.hintPl = QStringLiteral("Backend Btrfs zezwala na Overwrite, lecz domyślnym wyborem pozostaje bezpieczne Pomiń.");
        s.hintEn = QStringLiteral("Btrfs backend permits Overwrite, while Skip remains the safe default.");
        s.plan.entries = {
            makeFile(QStringLiteral("Report.txt"), QStringLiteral("/demo/source/Report.txt"), QStringLiteral("/demo/target/Report.txt"), 8192, 4096),
            makeFile(QStringLiteral("Summary.txt"), QStringLiteral("/demo/source/Summary.txt"), QStringLiteral("/demo/target/Summary.txt"), 2048, 1024),
        };
        break;
    case 6:
        s.id = QStringLiteral("G");
        s.titlePl = QStringLiteral("G. NTFS z niezweryfikowanym Overwrite (Zastąp widoczne, lecz disabled)");
        s.titleEn = QStringLiteral("G. NTFS with unverified Overwrite (Overwrite visible but disabled)");
        s.hintPl = QStringLiteral("Opcja Zastąp jest widoczna jako nieaktywna z wyjaśnieniem bezpieczeństwa; Zmień nazwę zapisze efektywną ścieżkę.");
        s.hintEn = QStringLiteral("Overwrite is visible as disabled with safety notice; Rename reports the effective destination path.");
        s.capabilities.canSafeOverwrite = false;
        s.capabilities.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");
        s.plan.entries = {
            makeFolder(QStringLiteral("Archive"), QStringLiteral("/demo/source/Archive"), QStringLiteral("/mnt/demo_ntfs/Archive")),
            makeFile(QStringLiteral("Archive/Config.ini"), QStringLiteral("/demo/source/Archive/Config.ini"), QStringLiteral("/mnt/demo_ntfs/Archive/Config.ini")),
        };
        break;
    default:
        s.id = QStringLiteral("H");
        s.titlePl = QStringLiteral("H. Długie ścieżki, polskie znaki i wiele konfliktów");
        s.titleEn = QStringLiteral("H. Long paths, Polish characters & many conflicts");
        s.hintPl = QStringLiteral("Elidowanie długich ścieżek z tooltipem, znaki Unicode (Zażółć gęślą jaźń) i licznik 6 konfliktów.");
        s.hintEn = QStringLiteral("Elided long paths with full tooltip, Polish Unicode names, and 6-conflict counter.");
        const QString longSeg = QStringLiteral("bardzo_długi_katalog_roboczy_").repeated(4);
        s.plan.entries = {
            makeFolder(QStringLiteral("Zażółć_Katalog"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog")),
            makeFolder(QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń")),
            makeFile(QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_1.txt")),
            makeFile(QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_2.txt"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_2.txt"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_2.txt")),
            makeFile(QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_3.txt"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_3.txt"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_3.txt")),
            makeFile(QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_4.txt"), QStringLiteral("/demo/źródło/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_4.txt"), QStringLiteral("/demo/cel/") + longSeg + QStringLiteral("Zażółć_Katalog/Gęślą_Jaźń/Dokument_Żółć_4.txt")),
        };
        break;
    }
    return s;
}

QString actionName(LocalMergeApprovedAction a, bool polish)
{
    switch (a) {
    case LocalMergeApprovedAction::Unapproved: return polish ? QStringLiteral("Niezatwierdzone") : QStringLiteral("Unapproved");
    case LocalMergeApprovedAction::Create: return polish ? QStringLiteral("Utwórz") : QStringLiteral("Create");
    case LocalMergeApprovedAction::MergeDirectory: return polish ? QStringLiteral("Scal folder") : QStringLiteral("MergeDirectory");
    case LocalMergeApprovedAction::Overwrite: return polish ? QStringLiteral("Zastąp") : QStringLiteral("Overwrite");
    case LocalMergeApprovedAction::Rename: return polish ? QStringLiteral("Zmień nazwę") : QStringLiteral("Rename");
    case LocalMergeApprovedAction::Skip: return polish ? QStringLiteral("Pomiń") : QStringLiteral("Skip");
    case LocalMergeApprovedAction::Abort: return polish ? QStringLiteral("Anuluj") : QStringLiteral("Abort");
    }
    return QStringLiteral("Unknown");
}

QPalette makeDarkPalette()
{
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(32, 34, 38));
    dark.setColor(QPalette::WindowText, QColor(235, 238, 242));
    dark.setColor(QPalette::Base, QColor(24, 26, 30));
    dark.setColor(QPalette::Text, QColor(235, 238, 242));
    dark.setColor(QPalette::Button, QColor(48, 52, 58));
    dark.setColor(QPalette::ButtonText, QColor(235, 238, 242));
    return dark;
}

QPalette makeLightPalette()
{
    QPalette light;
    light.setColor(QPalette::Window, QColor(245, 246, 248));
    light.setColor(QPalette::WindowText, QColor(24, 26, 30));
    light.setColor(QPalette::Base, QColor(255, 255, 255));
    light.setColor(QPalette::Text, QColor(24, 26, 30));
    light.setColor(QPalette::Button, QColor(232, 234, 238));
    light.setColor(QPalette::ButtonText, QColor(24, 26, 30));
    return light;
}

int dumpScreenshots(const QString &outDir)
{
    QDir().mkpath(outDir);
    const QLocale pl(QLocale::Polish, QLocale::Poland);
    const QLocale en(QLocale::English, QLocale::UnitedStates);

    const auto folderEntry = makeFolder(
        QStringLiteral("DemoData/Profiles"),
        QStringLiteral("/demo/source/DemoData/Profiles"),
        QStringLiteral("/demo/target/DemoData/Profiles"));
    const auto fileEntry = makeFile(
        QStringLiteral("DemoData/Profiles/Default/Settings.cfg"),
        QStringLiteral("/demo/source/DemoData/Profiles/Default/Settings.cfg"),
        QStringLiteral("/mnt/demo_ntfs/DemoData/Profiles/Default/Settings.cfg"),
        8192, 4096);

    LocalMergeBackendCapabilities ntfsCaps;
    ntfsCaps.canSafeOverwrite = false;
    ntfsCaps.filesystemLabel = QStringLiteral("NTFS-3G (FUSE)");

    auto saveWidget = [&](QWidget &w, const QString &name) {
        w.adjustSize();
        w.show();
        QApplication::processEvents();
        w.grab().save(QDir(outDir).filePath(name));
        w.hide();
    };

    QApplication::setPalette(makeDarkPalette());
    {
        LocalFolderMergeConflictDialog fPl(folderEntry, {}, false, pl, nullptr, 2, {2, 5});
        saveWidget(fPl, QStringLiteral("compact_folder_pl_dark.png"));
        LocalFileConflictResolutionDialog nPl(fileEntry, ntfsCaps, 3, {}, pl, nullptr, {4, 5});
        saveWidget(nPl, QStringLiteral("compact_file_ntfs_pl_dark.png"));
        LocalFolderMergeConflictDialog fEn(folderEntry, {}, false, en, nullptr, 2, {2, 5});
        saveWidget(fEn, QStringLiteral("compact_folder_en_dark.png"));
        LocalFileConflictResolutionDialog nEn(fileEntry, ntfsCaps, 3, {}, en, nullptr, {4, 5});
        saveWidget(nEn, QStringLiteral("compact_file_ntfs_en_dark.png"));
    }

    QApplication::setPalette(makeLightPalette());
    {
        LocalFolderMergeConflictDialog fPl(folderEntry, {}, false, pl, nullptr, 2, {2, 5});
        saveWidget(fPl, QStringLiteral("compact_folder_pl_light.png"));
        LocalFileConflictResolutionDialog nPl(fileEntry, ntfsCaps, 3, {}, pl, nullptr, {4, 5});
        saveWidget(nPl, QStringLiteral("compact_file_ntfs_pl_light.png"));
        LocalFolderMergeConflictDialog fEn(folderEntry, {}, false, en, nullptr, 2, {2, 5});
        saveWidget(fEn, QStringLiteral("compact_folder_en_light.png"));
        LocalFileConflictResolutionDialog nEn(fileEntry, ntfsCaps, 3, {}, en, nullptr, {4, 5});
        saveWidget(nEn, QStringLiteral("compact_file_ntfs_en_light.png"));
    }
    return 0;
}

class PreviewWindow final : public QWidget
{
public:
    explicit PreviewWindow(QWidget *parent = nullptr) : QWidget(parent), m_systemPalette(QApplication::palette())
    {
        resize(760, 520);

        auto *root = new QVBoxLayout(this);
        m_infoLabel = new QLabel(this);
        m_infoLabel->setWordWrap(true);
        root->addWidget(m_infoLabel);

        auto *ctrlRow = new QHBoxLayout();
        m_scenarioCombo = new QComboBox(this);
        for (int i = 0; i < 8; ++i) {
            m_scenarioCombo->addItem(buildScenario(i).titlePl);
        }
        m_langCombo = new QComboBox(this);
        m_langCombo->addItems({QStringLiteral("Polski (PL)"), QStringLiteral("English (EN)")});
        m_themeCombo = new QComboBox(this);
        m_themeCombo->addItems({QStringLiteral("System KDE"), QStringLiteral("Dark"), QStringLiteral("Light")});

        ctrlRow->addWidget(m_scenarioCombo, 2);
        ctrlRow->addWidget(m_langCombo);
        ctrlRow->addWidget(m_themeCombo);
        root->addLayout(ctrlRow);

        m_hintLabel = new QLabel(this);
        m_hintLabel->setWordWrap(true);
        root->addWidget(m_hintLabel);

        m_runBtn = new QPushButton(this);
        m_runBtn->setDefault(true);
        root->addWidget(m_runBtn);

        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        root->addWidget(m_log, 1);

        connect(m_scenarioCombo, &QComboBox::currentIndexChanged, this, [this] { updateHint(); });
        connect(m_langCombo, &QComboBox::currentIndexChanged, this, [this] { updateLabels(); });
        connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this] { applyTheme(); });
        connect(m_runBtn, &QPushButton::clicked, this, [this] { runCurrentScenario(); });

        updateLabels();
    }

private:
    void updateLabels()
    {
        const bool pl = m_langCombo->currentIndex() == 0;
        setWindowTitle(pl
            ? QStringLiteral("ThisPC Stage 3A — Podgląd kompaktowych dialogów konfliktów (BEZ ZAPISU NA DYSK)")
            : QStringLiteral("ThisPC Stage 3A — Compact Conflict Dialog Preview (NO DISK I/O)"));
        m_infoLabel->setText(pl
            ? QStringLiteral("<b>Tryb demonstracyjny Stage 3A (zero operacji na plikach)</b><br>"
                             "Wybierz scenariusz (A–H), język (PL/EN) oraz motyw (System KDE / Dark / Light) i uruchom rzeczywiste dialogi.")
            : QStringLiteral("<b>Stage 3A Demonstration Mode (zero filesystem I/O)</b><br>"
                             "Choose a scenario (A–H), language (PL/EN), and theme (System KDE / Dark / Light), then launch the real dialogs."));
        m_runBtn->setText(pl ? QStringLiteral("Uruchom scenariusz") : QStringLiteral("Run Scenario"));
        m_themeCombo->setItemText(0, pl ? QStringLiteral("Motyw systemowy KDE") : QStringLiteral("System KDE"));
        m_themeCombo->setItemText(1, pl ? QStringLiteral("Ciemny (Dark)") : QStringLiteral("Dark"));
        m_themeCombo->setItemText(2, pl ? QStringLiteral("Jasny (Light)") : QStringLiteral("Light"));

        for (int i = 0; i < 8; ++i) {
            const auto sc = buildScenario(i);
            m_scenarioCombo->setItemText(i, pl ? sc.titlePl : sc.titleEn);
        }
        updateHint();
    }

    void updateHint()
    {
        const bool pl = m_langCombo->currentIndex() == 0;
        const auto sc = buildScenario(m_scenarioCombo->currentIndex());
        m_hintLabel->setText(pl ? sc.hintPl : sc.hintEn);
    }

    void applyTheme()
    {
        const int idx = m_themeCombo->currentIndex();
        if (idx == 0) {
            QApplication::setPalette(m_systemPalette);
        } else if (idx == 1) {
            QApplication::setPalette(makeDarkPalette());
        } else {
            QApplication::setPalette(makeLightPalette());
        }
    }

    void runCurrentScenario()
    {
        const bool pl = m_langCombo->currentIndex() == 0;
        DemoScenario sc = buildScenario(m_scenarioCombo->currentIndex());
        const QLocale loc = pl
            ? QLocale(QLocale::Polish, QLocale::Poland)
            : QLocale(QLocale::English, QLocale::UnitedStates);

        const LocalMergeResolutionResult res = LocalMergeConflictResolver::resolve(
            sc.plan, sc.capabilities,
            [&](const LocalMergePlanEntry &entry, qsizetype remFolders, LocalMergeConflictProgress prog) {
                LocalFolderMergeConflictDialog dlg(entry, {}, false, loc, this, remFolders, prog);
                dlg.exec();
                return dlg.decision();
            },
            [&](const LocalMergePlanEntry &entry, const LocalMergeBackendCapabilities &caps, qsizetype remFiles, LocalMergeConflictProgress prog) {
                LocalFileConflictResolutionDialog dlg(entry, caps, remFiles, {}, loc, this, prog);
                dlg.exec();
                return dlg.decision();
            });

        QString out;
        out += QStringLiteral("=== %1 %2 (%3) ===\n")
            .arg(pl ? QStringLiteral("Scenariusz") : QStringLiteral("Scenario"),
                 sc.id,
                 res.cancelled
                     ? (pl ? QStringLiteral("ANULOWANO") : QStringLiteral("CANCELLED"))
                     : (pl ? QStringLiteral("ZAKOŃCZONO") : QStringLiteral("COMPLETED")));
        out += (pl
            ? QStringLiteral("Okna folderów: %1 | Okna plików: %2 | Bezkonfliktowe CreateNew: %3 | Pominięte poddrzewa: %4 | Przemapowane po zmianie nazwy: %5\n")
            : QStringLiteral("Folder prompts: %1 | File prompts: %2 | Auto-approved CreateNew: %3 | Subtrees skipped: %4 | Renamed remapped: %5\n"))
            .arg(res.folderPromptsShown)
            .arg(res.filePromptsShown)
            .arg(res.nonConflictingAutoApproved)
            .arg(res.subtreesSkipped)
            .arg(res.renamedSubtreeEntriesRemapped);

        for (const auto &e : std::as_const(sc.plan.entries)) {
            const bool needsReval = res.entriesRequiringStage3BRevalidation.contains(e.relativePath);
            const LocalMergeEntryResolutionView view = resolveEntryDestinationView(e, needsReval);
            if (e.approvedAction == LocalMergeApprovedAction::Rename) {
                out += (pl
                    ? QStringLiteral("  - %1 -> %2 (nowa nazwa: %3 | efektywny cel: %4 | pierwotny cel: %5)\n")
                    : QStringLiteral("  - %1 -> %2 (new name: %3 | effective dest: %4 | original dest: %5)\n"))
                    .arg(e.relativePath, actionName(e.approvedAction, pl),
                         view.approvedNewName, view.effectiveDestination, view.originalConflictDestination);
            } else {
                out += (pl
                    ? QStringLiteral("  - %1 -> %2 (efektywny cel: %3)\n")
                    : QStringLiteral("  - %1 -> %2 (effective dest: %3)\n"))
                    .arg(e.relativePath, actionName(e.approvedAction, pl), view.effectiveDestination);
            }
        }
        if (!res.entriesRequiringStage3BRevalidation.isEmpty()) {
            out += pl ? QStringLiteral("Wymaga ponownej walidacji w Stage 3B:\n")
                      : QStringLiteral("Requires Stage 3B revalidation:\n");
            for (const QString &r : res.entriesRequiringStage3BRevalidation) {
                out += QStringLiteral("  * %1\n").arg(r);
            }
        }
        m_log->setPlainText(out);
    }

    QPalette m_systemPalette;
    QLabel *m_infoLabel = nullptr;
    QComboBox *m_scenarioCombo = nullptr;
    QComboBox *m_langCombo = nullptr;
    QComboBox *m_themeCombo = nullptr;
    QLabel *m_hintLabel = nullptr;
    QPushButton *m_runBtn = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int dumpIdx = args.indexOf(QStringLiteral("--dump-screenshots"));
    if (dumpIdx >= 0 && dumpIdx + 1 < args.size()) {
        return dumpScreenshots(args.at(dumpIdx + 1));
    }
    PreviewWindow win;
    win.show();
    return app.exec();
}
