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

### 0.28.0 — Batch Rename ✅ wydane i zainstalowane (wariant A)

- Prefix/suffix, numerowanie, znajdź/zastąp, regex, zmiany rozszerzeń/wielkości liter oraz podgląd i diagnostyka planu.
- Lokalne łańcuchy i zamiany nazw; obsługa cykli 3+ jest wieloetapowa, a nie atomowa dla całej partii.
- Grupowe Undo/Redo w obsługiwanych scenariuszach; asynchroniczne wykonywanie swapów i blokada współpracujących modyfikacji między kolejnymi wymianami.
- Ochrona przed nadpisaniem, weryfikacja tożsamości plików i ponowny preflight.
- Testy wydania: build PASS, focused 2234 asercje PASS (w tym Batch Rename 1790), pełna regresja 23 zestawy / 5409 asercji PASS, `git diff --check` PASS.
- Odbiór użytkownika po instalacji: bez zgłoszonych błędów w prefix, suffix, znajdź/zastąp i Ctrl+Z dla wielu plików. **Nie oznacza to pełnego ręcznego odbioru wszystkich scenariuszy GUI.**
- Produkcyjne automatyczne recovery v2 obejmuje tylko kwalifikujące się lokalne operacje liniowe Execute/Undo/Redo. Automatyczne recovery swapów i cykli nadal nie jest włączone; brak gwarancji na zanik zasilania, zdalne URL-e/KIO i działania niekooperujących procesów.
- Szczegółowy zakres i ograniczenia: `RELEASE_NOTES_0.28.0.md`; przypadki i status testów: `TEST_CHECKLIST.md`; historia etapów: `docs/BATCH_RENAME_STAGE1.md`, `docs/BATCH_RENAME_STAGE2.md`, `docs/BATCH_RENAME_STAGE3.md` i `docs/BATCH_RENAME_CRASH_RECOVERY_STAGE*.md`.

## Planned releases

### 0.29.0 — Inteligentne tworzenie plików i folderów ✅ wydane i zainstalowane

**Cel:** przy tworzeniu nowego elementu wykrywać zajętą nazwę przed otwarciem okna i od razu wstawiać pierwszą wolną propozycję do pola nazwy.

**Stan wydania:** Stage 1 lokalny i Stage 2 zdalnego KIO są zaimplementowane, a ich
podstawowy ręczny przepływ został potwierdzony przez użytkownika. Stage 2 używa
dwóch asynchronicznych listingów i końcowych operacji bez `Overwrite` ani
`Resume`. Nie oznacza to testu awarii połączenia, wszystkich protokołów ani
atomowej rezerwacji; granice opisuje `docs/NEW_ITEM_NAMING_STAGE2.md`. Wersja
0.29.0 została wydana jako commit `e56acee`, oznaczona tagiem `v0.29.0`,
zainstalowana i ręcznie potwierdzona przez użytkownika. Pełna regresja wydania:
23 zestawy / 5430 asercji PASS.

- Dla zajętej nazwy proponować `Nowy dokument (1).txt`, następnie `(2)`, `(3)` itd.; bez kolizji pozostawiać nazwę domyślną.
- Numer umieszczać przed rozszerzeniem; poprawnie traktować nazwy katalogów, szablony, pliki z wieloma kropkami oraz pliki ukryte (np. `.gitignore`).
- Wpisywać propozycję w edytowalne pole jeszcze przed zatwierdzeniem. Po ręcznej edycji nie zastępować wpisanej nazwy bez wiedzy użytkownika.
- Przed utworzeniem ponownie sprawdzać kolizję i stosować operację niewymuszającą nadpisania. Jeśli inny proces zajmie nazwę w międzyczasie, przedstawić nową propozycję lub czytelny konflikt; nigdy nie nadpisywać automatycznie.
- Uwzględnić zarówno pliki, foldery, jak i pozycje tworzone z szablonów, o ile ich backend zapewnia bezpieczną obsługę konfliktu. Dla zdalnych KIO osobno określić zachowanie i ograniczenia zamiast zakładać gwarancje lokalnego systemu plików.
- Działać względem właściwego katalogu i aktywnego panelu Split View, niezależnie od trybu ikon/listy/szczegółów.
- Testy: brak konfliktu, luki w numeracji, zajęte kolejne numery, rozszerzenia i Unicode, foldery, szablony, dowiązania symboliczne, ręczna edycja, jednoczesne utworzenie nazwy przez inny proces, oba panele i brak utraty istniejących danych.
- Kryterium odbioru: użytkownik widzi wolną nazwę od razu, może ją zmienić, a żaden test kolizji nie nadpisuje istniejącego elementu.

### 0.30.0 — Grouping and per-folder view settings — zakres zakończony, RC

**Stage 1 — per-folder view mode: zaimplementowany i ręcznie zaakceptowany**
- zapamiętywanie Icons/List/Details według znormalizowanego URL katalogu;
- wspólna preferencja dla panelu głównego, Split View, kart i restore session;
- lokalne i zdalne URL-e KIO, bez skanowania zawartości i bez zależności od metadanych;
- bezpieczny fallback do dotychczasowego ustawienia globalnego;
- focused regression po Stage 1b: 15 asercji ustawień + 300 asercji paneli/menu PASS;
- szczegóły i granice: `docs/GROUPING_VIEW_SETTINGS_STAGE1.md`.

**Stage 1b — porządkowanie menu Widok: zaimplementowany i ręcznie zaakceptowany**
- Icons/List/Details pozostają jedną grupą radio i są routowane do aktywnego panelu;
- podmenu Pokaż zawiera wyłącznie działające opcje: ukryte elementy, miniatury,
  panel podglądu i pełne nazwy, z zachowaniem stanów oraz skrótów Ctrl+H/Alt+P;
- ten sam układ obowiązuje w menu kontekstowym Widok; Sort, Search, KIO, karty,
  Split View, sesja i zapamiętywanie per folder nie zmieniają kontraktu.
- build PASS; pełna regresja: 24 zestawy / 5456 asercji PASS.

**Stage 1c — rozmiary ikon: zaimplementowany i ręcznie zaakceptowany**
- cztery rzeczywiste poziomy: Bardzo duże 96 px, Duże 64 px, Średnie 48 px i Małe 32 px;
- Duże zachowuje dotychczasową geometrię i jest bezpiecznym fallbackiem;
- rozmiar jest zapamiętywany według znormalizowanego URL folderu i współdzielony
  przez panel główny, Split View, karty oraz restore session;
- menu główne i kontekstowe routują zmianę do aktywnego panelu, a radio state
  podąża za panelem; Lista/Szczegóły zachowują dotychczasowe rozmiary;
- każda wielkość ikon ma stałą siatkę; callout zaznaczonej pełnej nazwy nie zmienia
  geometrii elementów ani położenia sąsiadów.
- build PASS; focused regression: 23 ustawień + 305 paneli/menu PASS;
  pełna regresja: 24 zestawy / 5469 asercji PASS.

**Stage 1d — widok kompaktowy: zaimplementowany i ręcznie zaakceptowany**
- czwarty tryb per-folder, dostępny w panelu głównym i Split View;
- gęste, jednowierszowe elementy z ikoną 20 px, w kolumnach wypełnianych od góry;
- stabilna szerokość komórki i elidowanie nazw zapewniają przewidywalny koszt układu;
- natywna nawigacja wizualna Qt, zaznaczanie, aktywacja, menu kontekstowe i DnD
  korzystają z tych samych ścieżek co Lista; callout pełnej nazwy pozostaje Icons-only.

**Suwak rozmiaru ikon — przyszłe oddzielne zadanie po 0.30, niezrealizowane**
- około 8–10 stopni, wyłącznie dla trybu Icons;
- ustawienie per-folder i dla aktywnego panelu, opcjonalnie Ctrl+wheel;
- suwak nie przełącza do List, Details ani Compact.

**Dalsze funkcje Widok — osobny backlog, nie Stage 2 grupowania**
- Tiles/Content wymagają nowych layoutów i testów wszystkich backendów;
- details pane jest odrębny od istniejącego Preview Pane;
- checkboxy wyboru i przełącznik rozszerzeń wymagają nowych zachowań modelu/nazw;
- panel nawigacji istnieje, ale nie ma jeszcze kompletnego przełącznika widoczności
  z trwałym stanem i bezpiecznym układem Split View.

**Nawigacja klawiaturą — przyszłe zadanie, niezrealizowane**
- strzałki działają obecnie w zwykłych folderach, ale nie w widoku `thispc:/`;
- Enter ma otwierać folder lub uruchamiać plik, Backspace przechodzić do katalogu
  nadrzędnego, Alt+Left/Alt+Right obsługiwać historię, a Alt+Up katalog nadrzędny;
- skróty mają działać w aktywnym panelu Split View, na kartach oraz w trybach
  Icons/List/Details/Compact, z poprawnym odtwarzaniem fokusu po nawigacji;
- pola Search i adresu, inline rename oraz dialogi muszą zachować własną obsługę
  klawiatury; implementacja nie może globalnie przechwytywać ich zdarzeń;
- testy mają objąć oba panele, przełączanie kart i trybów, historię, granice historii,
  fokus oraz lokalne i KIO URL-e. Ten wpis nie oznacza implementacji funkcji.

**Stage 2 — grouping: zaimplementowany i ręcznie zaakceptowany**
- Group by Type używa rzeczywistych kategorii modelu KDE w Icons/List/Compact
  oraz nieselektowalnych wierszy sekcji w Details; nagłówki nie mają URL-a ani
  roli pliku i nie trafiają do akcji, schowka, DnD, Preview, Quick Look ani menu elementu;
- `Brak/Typ/Data/Rozmiar` jest zapamiętywane per znormalizowany URL i działa dla
  panelu głównego, Split View, kart, Search/source URLs, KIO i restore session;
- bieżące sortowanie jest wykonywane przed stabilnym podziałem na grupy, więc
  kolejność wewnątrz każdej sekcji pozostaje zgodna z kluczem i kierunkiem sortowania;
- Group by Date używa czasu modyfikacji z metadanych KIO i rozłącznych grup
  lokalnego kalendarza: Future, Today, Yesterday, This week, Last week,
  wcześniejsze zakresy oraz Unknown date; tydzień zaczyna się w poniedziałek;
- osobny timer każdego panelu przelicza kategorie daty po następnej lokalnej
  północy, także przez dni DST 23/25 h, bez synchronicznego skanowania;
- Group by Size używa `UDS_SIZE` już dostarczonego przez KIO. Granice są binarne,
  rozłączne i udokumentowane; foldery oraz nieznane rozmiary mają osobne grupy;
- grouping nie wykonuje rekurencyjnego skanowania, synchronicznego `stat` ani
  dodatkowych zdalnych żądań KIO;
- ręczny odbiór Type, Date i Size potwierdził Icons/List/Details/Compact,
  Split View, Search, PPM nagłówków, sortowanie, trwałość per-folder i selekcję;
- poprawiono obrys zaznaczenia i zalegający hover w widoku pogrupowanym. Logi
  diagnostyczne potwierdziły pojedynczą selekcję modelu; wizualny „duch” był
  stale `State_MouseOver` i został usunięty przez wyliczanie hover z pozycji kursora;
- weryfikacje po wycinkach: Type 24/6197 PASS, Date 24/6213 PASS; Size build,
  test granic/ustawień 190 asercji i pełna regresja 24 zestawów PASS;
- finalny RC po bumpie wersji: build PASS, focused 778 asercji PASS, pełne
  `--all` 24 zestawy / 6278 asercji PASS, `git diff --check` i audit wersji PASS.
  Pozostaje czyste archiwum + SHA-256 oraz osobna zgoda na commit/tag/install.

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

## Backlog bez przypisanej wersji

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

### Advanced search — przeniesione z planu 0.29.0 (wersja do ustalenia)
- type/name/extension/date/size filters;
- files-only/folders-only;
- saved searches;
- Baloo acceleration when available, current search fallback otherwise.

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
