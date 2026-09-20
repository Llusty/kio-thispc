# ROADMAP.md

This ordering prioritizes core file-manager correctness, safety and maintainability before advanced convenience features.

## Completed baseline
- **0.16.0.1 — Undo / Redo** ✅ user-confirmed
  - copy/move/rename/trash/new folder/new file where supported by KIO;
  - Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z;
  - rename hotfix using `KIO::moveAs()` + `recordCopyJob()`.
- **0.17.0.5 — Full Drag & Drop + Split View parity** ✅ user-confirmed
  - background/folder/tab drops, primary ↔ split, Ctrl=Copy, Shift=Move;
  - tab hover activation;
  - active-pane shortcuts/context menus;
  - cycle protection and Undo integration.

### 0.18.0 — Copy/move conflict handling ✅ user-confirmed
- interactive KIO/KIOWidgets conflict UI for clipboard, Drag & Drop and Send To;
- overwrite / skip / rename or suggested new name;
- remembered decisions for multiple conflicts;
- source/destination size and timestamp comparison;
- nested conflicts handled by `KIO::CopyJob`;
- clean cancellation without a second error dialog.

## Completed releases

### 0.19.0.4 — Session restore + full names ✅ user-confirmed
- restore tabs, active tab, locations and primary navigation history;
- restore Split View per tab, including right-pane location/view/sort state;
- restore splitter widths;
- option in View to reopen the previous session;
- selected icon expands to show its full wrapped name;
- toolbar toggle `Full names` / `Pełne nazwy` persists across launches;
- fixed rigid IconMode grid and bounded multi-line filename layout;
- selected full-name callout no longer changes row geometry.

### 0.20.0 — Quick Access / Favorites / Recent ✅ user-confirmed
- pin/unpin folders;
- drag reorder with persistent ordering;
- favorites / Quick Access in sidebar;
- persistent recent locations;
- optional recent files deferred.

### 0.21.0 — Architecture refactor + stabilization ✅ user-confirmed
- extracted `DirectoryView`, `SplitBrowserPane`, `Sidebar`, `SessionManager`, `OperationManager`, `UndoController`, `PropertiesDialog`, `SearchController` and `FileActions`;
- moved shared browser helpers/types to `browsercommon.h`;
- reduced duplication between primary and split panes;
- reduced `src/thispcview.cpp` from roughly 14,178 to roughly 7,250 lines;
- added/expanded regression tests for pane actions, tab Drag & Drop, Properties, search and file actions;
- 577 automated assertions pass;
- full manual KDE/CachyOS acceptance passed without regressions.

### 0.22.0 — Advanced transfer window ✅ user-confirmed
- normal movable/minimizable window opens automatically for active operations;
- current file, source/destination summary, processed/total size, prominent percentage, current/average speed and ETA;
- bounded speed-over-time graph;
- multiple simultaneous operations in one dynamically sized window;
- responsive expandable details and per-operation cancellation;
- automatic close after the last active operation finishes;
- compact popup remains the operation-history view;
- misleading KIO-based pause was rejected during manual acceptance and removed.

### 0.23.0 — Native local transfer engine ✅ user-confirmed
- chunked local copy with real pause/resume at a known byte offset;
- cross-device move built on verified copy followed by source removal;
- safe partial-file handling and cleanup/recovery after cancellation or failure;
- conflicts, overwrite/rename/skip decisions, directories, links, permissions and timestamps;
- disk-full, disconnect and I/O error handling;
- progress integration with the 0.22.0 detailed window and compact history;
- Undo/Redo integration where the completed operation is safely reversible;
- keep KIO for remote URLs and operations where it remains the appropriate backend;
- integrity and interruption tests before replacing any existing local KIO path;
- full automated regression: **842/842 assertions**;
- full manual KDE/CachyOS acceptance passed, including conflicts, trees, symlinks, metadata, Pause/Resume, Undo/Redo, disk-full, device-loss and SHA-256 integrity.

### 0.24.0 — Sidebar & Split View UX ✅ user-confirmed
- equal Split View panes with aligned per-pane breadcrumbs and one shared toolbar/sidebar routed to the active pane;
- Sidebar Drag & Drop into supported Places, devices and Quick Access destinations;
- vertically scrollable, resizable sidebar with persisted user-selected width and long-label ellipsis/tooltips;
- independent Search state, results, filters, progress and jobs for both panes;
- full `thispc:/` / This PC card presentation in both panes;
- full automated regression: **1022/1022 assertions**;
- full Stage 1–4 manual KDE/CachyOS acceptance passed.

## 0.25.0 — Archives + richer New menu ✅ user-confirmed

### Zakres wykonany
Archives:
- native KDE/Ark-oriented integration;
- ZIP / 7z / tar.gz where backend support exists;
- Extract Here / Extract To / Add to archive;
- CLI fallback only where justified.

New:
- folder, empty file, text, Markdown;
- user templates;
- Empty Trash with confirmation;
- Split View width fix for long paths;
- Stage 3 automated regression: 20 suites / 3470 assertions PASS; manual acceptance.

### 0.26.0 — Preview Pane ✅ user-confirmed

- image preview and proportional scaling;
- text, Markdown, JSON and XML;
- asynchronous first-page PDF preview using QtPdf;
- opaque white PDF background fix;
- audio/video metadata using TagLib;
- image EXIF metadata using Exiv2;
- safe archive manifest preview using libarchive;
- folder summaries;
- Alt+P preview toggle;
- Split View breadcrumb improvements;
- protection against stale asynchronous results;
- full automated regression: 20 suites PASS;
- Preview Pane: 106 assertions PASS;
- Split Layout: 99 assertions PASS;
- manual KDE/CachyOS acceptance confirmed.

### 0.27.0 — Quick Look on Space ✅ user-confirmed

- large temporary preview without opening the associated app;
- Space opens/closes Quick Look only from the active file view; Esc closes it;
- selection and active Split View pane changes update the preview while file-view focus stays intact;
- Search, address editing, inline rename, popups and dialogs retain normal Space behavior;
- shared Preview Pane backend, supported formats, limits and stale-result protection;
- Alt+P Preview Pane remains independent;
- full automated regression: **22 suites / 3598 assertions PASS**;
- focused assertions: Quick Look 19, Preview Pane 106, pane routing 269 and Split Layout 99;
- all 9 manual KDE/CachyOS acceptance cases confirmed by the user.

## Planned releases

## 0.28.0 — Batch rename (zakres zamknięty)
- Stage 1: prefix/suffix, numbering, bezpieczny preview i Undo per element — 8/8 testów ręcznych potwierdzone;
- Stage 2: replace text, extension/case changes, regex i diagnostyka preview; ręczne punkty 1–8 i 10 PASS, w tym no-op, kolizje/race, oba panele Split View i regresje. Historyczny test anulowania większej partii KIO w trakcie nie został wykonany i nie opisuje już zwykłych kwalifikujących się partii lokalnych;
- zachowana ochrona przed nadpisaniem, ponowny preflight i aktywny panel Split View;
- Stage 3A: acykliczne łańcuchy, snapshot `lstat` i preflight; build i 529 asercji focused PASS, ręczny test łańcucha i zawartości PASS; pełnej regresji po Stage 3A jeszcze nie potwierdzono;
- Stage 3B.1: Linux `renameat2(RENAME_EXCHANGE)` dla dwóch nazw bez Undo; build/focused/full regression PASS, pojedynczy ręczny test zawartości `ab ↔ ba` PASS (reszta checklisty nadal otwarta);
- Stage 3B.2: patch do weryfikacji — cykle 3+ jako N−1 osobnych atomowych wymian, kontrola inode i odwracanie wykonanych etapów po zwykłym błędzie/anulowaniu; **nie jest to atomowość całej partii ani trwałe recovery**;
- Stage 3B.2: pełna regresja i ręczny cykl 3 plików z zachowaniem danych potwierdzone przez użytkownika; pozostała checklista otwarta.
- Stage 3C.1: diagnostyczny dziennik cykli 3+; po naprawie sprawdzenia pending journal pełna regresja `FULL_REGRESSION_EXIT=0`, Batch Rename 163 PASS; ręczny cykl trzech plików zachował treść i nie pozostawił `cycle-*.json`. Automatyczne recovery nadal nie istnieje.
- Stage 3C.2A (patch do lokalnej weryfikacji): pojedyncze Undo/Redo dla jednej dwuelementowej wymiany; **nie** obejmuje mieszanych partii, wielu swapów ani cykli 3+. Szczegóły: `docs/BATCH_RENAME_STAGE3.md`.
- Stage 3C.2A: build / 175 asercji Batch Rename / pełna regresja PASS; użytkownik potwierdził jedno Ctrl+Z i Ctrl+Y na zamianie dwóch plików.
- Stage 3C.2B.1 (nowy patch do testów): jedno journalowane Undo/Redo dla dokładnie jednego izolowanego cyklu 3+; nieatomowe N−1 wymian. Łańcuchy i partie mieszane nadal bez wspólnego Undo.
- Stage 3C.2B.1: build, pełna regresja i ręczny cykl trzech plików z pojedynczym Ctrl+Z / Ctrl+Y oraz kontrolą zawartości: PASS według użytkownika.
- Stage 3C.2B.2A: wspólne, dziennikowane Undo/Redo dla czystych lokalnych łańcuchów, również kilku niezależnych łańcuchów w jednym planie; ręczny test dwóch łańcuchów, wspólnego Undo/Redo, race oraz wykonania z aktywnego lewego panelu Split View PASS. Replay jest nieatomowy, bez Cancel w trakcie i wymaga ręcznej kontroli journalu po awarii.
- Ręcznie PASS także: swap i cykl 3 z Undo/Redo, odmowa Redo po podmianie inode z trwałym komunikatem ok. 12 s oraz cancel przed startem. Stan audytowany 2026-09-19; ostatnia zapisana pełna regresja, wykonana **przed tymi nowymi testami ręcznymi**, to 23 zestawy / 3865 asercji PASS; nie jest to świeższy rerun.
- Przed wydaniem pozostają wyłącznie jawne pozycje z bieżącego audytu w `TEST_CHECKLIST.md`; automatyczne crash recovery, atomowość wieloetapowej partii i zdalne URL-e nie są obietnicą 0.28.0.
- Różnice wizualne trybów widoku między lewym i prawym panelem Split View: osobne przyszłe zadanie, poza zakresem 0.28.0, o ile nie ujawnią błędu funkcjonalnego.

Zakres wydania kończy się na produkcyjnym auto-recovery v2 dla kwalifikujących
się lokalnych operacji linear Execute/Undo/Redo. Ograniczenia są częścią
kontraktu wydania, nie ukrytymi kryteriami blokującymi; pełna lista znajduje się
w `RELEASE_NOTES_0.28.0.md`.

### Po 0.28.0 — recovery i domknięcie Batch Rename (wersja do ustalenia)

- 4A.3: połączyć zweryfikowany izolowany silnik swap v2 z prawdziwymi
  produkcyjnymi kontrolerami Execute/Undo/Redo, workerami, statusami i gate;
- 4B: osobno aktywować generator i startupową allowlistę swapu dopiero po
  black-box Execute/Undo/Redo, drugim restarcie, dwóch instancjach i audycie GUI;
- zaprojektować osobny bezpieczny protokół recovery dla cykli 3+; nie traktować
  istniejącego nieatomowego N−1 replay jako pełnej atomowości;
- określić i przetestować gwarancje KIO fallback oraz zdalnych URL-i, w tym
  kontrolowane anulowanie w trakcie na jednorazowych danych;
- power-loss badać wyłącznie w disposable VM/FS-image, nigdy na danych
  użytkownika; SIGKILL pozostaje testem awarii procesu;
- kontynuować ochronę przed procesami niekooperującymi i TOCTOU, bez obiecywania
  gwarancji niemożliwych dla bezwarunkowego `RENAME_EXCHANGE`;
- ręcznie sprawdzić różnice wizualne trybów lewego/prawego panelu Split View.

### 0.29.0 — Advanced search
- type/name/extension/date/size filters;
- files-only/folders-only;
- saved searches;
- Baloo acceleration when available, current search fallback otherwise.

### 0.30.0 — Grouping and per-folder view settings
- group by type/date/size;
- Today/Yesterday/This week/etc.;
- remember icon/list/details mode per folder.

### 0.31.0 — Split View synchronization
- compare left/right;
- same / only-left / only-right / changed;
- preview synchronization plan;
- copy differences left/right;
- safe sync execution.

### 0.32.0 — File/folder comparison
- external Meld/KDiff3 integration first;
- folder difference view;
- optional hashes for stronger comparisons.

### 0.33.0 — Drives and devices
- mount/unmount/eject;
- removable media;
- MTP;
- ISO mount/unmount;
- filesystem/mount details;
- sensible SMART integration where available.

### 0.34.0 — Network
- SMB, SFTP, FTP, WebDAV via KIO where appropriate;
- saved remote locations;
- network discovery where reliable.

### 0.35.0 — Disk usage analyzer
- biggest directories/files;
- background scan;
- top-N views;
- optional treemap later.

### 0.36.0 — Duplicates + checksums
- SHA-256 / SHA-1 / MD5 utilities;
- duplicate discovery by size then hash;
- safe review before removal/move.

### 0.37.0 — Advanced Properties / ACL
- POSIX ACL;
- owner/group/inode/filesystem/mount;
- atime/mtime/ctime;
- MIME/checksum;
- EXIF/media metadata;
- continue verified NTFS behavior rather than blanket assumptions.

### 0.38.0 — Administrator fallback for failed operations
When a normal operation receives permission denied, offer a targeted `admin://` retry instead of requiring an entire window to run elevated.

### 0.39.0 — Transfer queue/control
- serial vs parallel;
- concurrency limit;
- priorities/order;
- queue-wide control and priorities on top of backends that genuinely support them.

### 0.40.0 — Notifications + operation history
- Plasma notification for long/background completions;
- recent operation log with source/destination/result;
- retry where meaningful.

### 0.41.0 — Plugin / Service Action architecture
Allow new context-menu actions and integrations without editing the core window source.

## Before 1.0
Stability-focused cycle covering:
- large files/directories;
- tens of thousands of entries;
- Btrfs/ext4/NTFS;
- local and remote KIO;
- Split View parity;
- Undo/Redo conflicts;
- device hotplug;
- crash recovery;
- memory/resource leaks;
- keyboard accessibility audit;
- regression tests for file-operation safety.
