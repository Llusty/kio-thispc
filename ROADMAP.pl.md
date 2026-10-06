[English](ROADMAP.md) | [Polski](ROADMAP.pl.md)

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

- **0.18.0 — Copy/move conflict handling** ✅ user-confirmed
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

### 0.25.0 — Archives + richer New menu ✅ user-confirmed

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

### 0.30.0 — Grouping and per-folder view settings ✅ wydane i zaakceptowane

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

**Suwak rozmiaru ikon — przyszłe oddzielne zadanie po 0.30.0, niezrealizowane**
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
- finalny baseline po bumpie wersji: build PASS, focused 778 asercji PASS, pełne
  `--all` 24 zestawy / 6278 asercji PASS, `git diff --check` i audit wersji PASS.
  Wydanie zostało zaakceptowane przez użytkownika.

### 0.31.0 — Split View synchronization ✅ user-confirmed

**Stage 1 — read-only pane comparison: zaimplementowany i zaakceptowany ✅**
- porównywanie bieżącego folderu lewego i prawego panelu Split View (tylko bezpośrednie dzieci, brak rekurencji);
- klasy wyniku: Same, Only left, Only right, Changed;
- dopasowanie nazw z zachowaniem wielkości liter i pełną obsługą Unicode;
- porównywanie metadanych KIO (rozmiar, czas modyfikacji mtime, typ wpisu); brak hashy;
- foldery o tej samej nazwie klasyfikowane jako Same na poziomie obecności w bieżącym katalogu (bez diffu rekurencyjnego);
- asynchroniczne listowanie KIO z unieważnianiem spóźnionych sygnałów i anulowaniem starych generacji;
- blokada porównywania dla `thispc:/` oraz wyników `thispcsearch:/` z czytelnym komunikatem;
- brak modyfikacji i mutacji plików (w 100% read-only);
- szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE1_COMPARE.md`.

**Stage 2 — sync plan preview: zaimplementowany i zaakceptowany ✅**
- czyste i deterministyczne planowanie akcji synchronizacji dla kierunków Lewy → Prawy oraz Prawy → Lewy;
- klasy planu: Bez zmian (NoAction), Kopiuj (CopyFile), Zaktualizuj (UpdateFile), Konflikt (Conflict), Nieobsługiwane (Unsupported);
- brak usuwania (brak Delete / brak mirror);
- brak rekurencyjnej synchronizacji katalogów (katalogi po obu stronach = NoAction, katalog źródłowy = Unsupported);
- brak dodatkowego I/O i hashy (bazuje na zaakceptowanym modelu CompareEntry);
- zachowanie wielokrotności duplikatów i informacji o brakujących metadanych;
- modalny dialog podglądu planu (SyncPlanPreviewDialog) powiązany z numerem generacji;
- w 100% read-only podgląd z przejściem do bezpiecznego wykonania Stage 3;
- szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE2_PLAN_PREVIEW.md`.

**Stage 3 — safe execution: zaimplementowany i zaakceptowany ✅**
- bezpieczne asynchroniczne wykonanie planu synchronizacji oparte o `LocalFileCopyJob`;
- wykonywane są wyłącznie operacje `CopyFile` i `UpdateFile`;
- **zero usuwania**: brak operacji Delete, brak flag mirror;
- **brak rekurencji**: brak synchronizacji poddrzew katalogów;
- wpisy konfliktowe (`Conflict`) i nieobsługiwane (`Unsupported`) pozostają w 100% nietknięte;
- atomowy preflight bezpośrednio przed mutacją każdego pliku (odrzucanie kropki, dwukropki, braków uprawnień);
- deterministyczny cykl życia Anulowania (Cancel): eliminacja wyścigu liczników (brak off-by-one accounting), oczekiwanie na zakończenie aktywnego zadania wątku roboczego i dokładna zgodność raportu UI ze stanem dysku;
- automatyczne ponowne porównanie (Re-compare) po zakończeniu synchronizacji;
- szczegóły i semantyka: `docs/SPLIT_SYNC_STAGE3_EXECUTION.md`.

**Wyniki testów i weryfikacji 0.31.0:**
- Focused `split_compare`: 291/291 asercji PASS;
- Focused pane actions (`split_layout`, `split_compare`, `local_transfer`, `transfer_plan`, `local_move`, `actions`, `operations`): 741 asercji PASS;
- Pełna regresja (`run-pane-actions.py --all`): 25 zestawów testowych / 6569 asercji PASS (100%; korekta arytmetyczna po wydaniu: 6278 + 291, tag bez zmian);
- Manualny test akceptacyjny Cancel (20 000 plików): raport 3330 skopiowano / 16670 anulowano / 0 błędów, na dysku dokładnie 3330 plików, brak plików tymczasowych/częściowych — PASS.

**0.31.0 — COMPLETE**

### 0.32.0 — Architecture Cleanup ✅ complete
- Split Sync dependencies made acyclic;
- application widgets/style extracted and test-harness seams hardened;
- explicit `PaneContext` / `PaneAdapter` contract;
- `TabController`, `SearchUiController` and `SelectionMenuController` extracted;
- primary listing/rendering state extracted into `PrimaryBrowserPane`;
- behavior preserved, with regression fixes only and no new user-facing features;
- final Stage 8 baseline: **26 suites / 6662 assertions PASS**;
- dalsze łączenie obu powłok UI w common `BrowserPane` pozostawiono do ponownej
  oceny w 0.33.0.

### 0.33.0 — Architecture Cleanup Continuation ✅ wydane, zainstalowane i potwierdzone przez użytkownika
- `LocationPresentation`, `NavigationHistory`, `PreviewCoordinator` and
  `ActionStateController` extracted;
- shared `DirectoryListingCore` used by Primary and Split;
- `PaneMenuController` and `DriveHomeCoordinator` extracted;
- drive-sidebar context-menu lifetime crash fixed;
- inline rename activation fixed without changing fast double-click open;
- final release baseline: **31 suites / 7852 assertions PASS**;
- release commit `a3710e466920542aa2a52ac90cdd9a4d3b5c2312`, tag `v0.33.0`;
- obecny model `DirectoryListingCore + osobne UI shells` jest zaakceptowanym
  końcowym rozwiązaniem architektonicznym. Wspólny `BrowserPane` nie jest już
  obowiązkowym przyszłym etapem; temat może wrócić wyłącznie wtedy, gdy konkretna
  nowa duplikacja uzasadni koszt i ryzyko kolejnego scalenia powłok UI.

### 0.34.0 — Explorer UX / View & Navigation Polish ✅ feature-complete, release prep
- Stage 2 ukończony: stabilne CLI `--version`, Split address/breadcrumb parity i
  ograniczone odstępy kart dysków;
- Stage 3 ukończony: czytelne wizualizacje ukrytych elementów;
- Stage 4 ukończony: wspólny inline rename dla F2, PPM → Rename i wolnego
  drugiego kliknięcia; Batch Rename i multi-select pozostają osobną ścieżką;
- Stage 5 ukończony: nawigacja klawiaturą oraz stabilny current/focus w
  `thispc:/`, z routingiem do aktywnego panelu;
- Stage 6 ukończony: profile widoku per-folder oraz snapshotowe Apply/Remove to
  subfolders, bez rekurencyjnego skanowania KIO i z pierwszeństwem jawnego
  override katalogu potomnego;
- Stage 7 ukończony: dziewięć stopni rozmiaru ikon, kontrolki menu oraz skróty
  `Ctrl++`, `Ctrl+=` i `Ctrl+-` routowane do aktywnego panelu;
- finalny Split View visual parity polish ukończony: wspólne nagłówki i metryki
  Icons/List/Details/Compact, deterministyczne kolumny Details, symetryczne
  profile per-URL oraz praktycznie identyczna runtime geometry adresu;
- nieblokujący cosmetic follow-up: około 1 px/sub-pixel różnicy pionowego
  wyrównania nagłówka/paska i położenia niebieskiej active-pane accent line
  względem szarej ramki; świadomie odłożone poza 0.34.0;
- nieblokujący future polish: ujednolicić kursor kart folderów i dysków
  `thispc:/`/Split Home z `PointingHandCursor` do standardowego `ArrowCursor`;
  świadomie odłożone poza 0.34.0;
- wykrywanie pendrive/removable pozostaje w 0.35.0 Drives & Devices;
- double-click separatora Split przywracający 50/50 pozostaje przyszłym pomysłem
  polishowym, nie zakresem 0.34.0;
- Pełna regresja: 32 zestawy / 8317 asercji PASS; CLI `thispc-view 0.34.0`, exit code 0: PASS.

### 0.35.0 — Drives & Devices ✅ feature-complete, wydane (v0.35.0)
- Stage 1 ukończony: dynamiczne wykrywanie dysków i urządzeń przez KDE Solid; sterowany zdarzeniami `SolidDeviceMonitor` z debouncingiem 250 ms eliminuje okresowy polling i miganie „Odświeżanie…”;
- Stage 2 ukończony: wykrywanie i prezentacja odmontowanych woluminów wymiennych na `thispc:/` jako „Niezamontowany”; montowanie na żądanie (`DeviceMountController`) po kliknięciu lub Enter z natychmiastowym wejściem do katalogu; sidebar prezentuje wyłącznie zamontowane woluminy;
- Stage 3 ukończony: pełna asynchroniczna obsługa cyklu życia urządzeń (`DeviceRemovalController`):
  - akcja „Odmontuj” wykonuje czyste odmontowanie systemu plików wyłącznie przez natywny QtDBus `org.freedesktop.UDisks2.Filesystem.Unmount` (brak wyłączania zasilania);
  - akcja „Bezpiecznie usuń” koordynuje odmontowanie wszystkich zamontowanych partycji dysku i asynchroniczny `org.freedesktop.UDisks2.Drive.PowerOff`;
  - obsługa bezpiecznego usuwania dla woluminów już odmontowanych;
  - obsługa wysuwania nośników optycznych („Wysuń”) na urządzeniach ze zdolnością `canEject`;
  - bezpieczne przekierowanie paneli Primary i Split z odmontowanego punktu montowania z powrotem do `thispc:/`.
- dla NTFS utrzymywać verified behavior zamiast blanket assumptions;
- Pełna regresja: 35 zestawów / 8562 asercje PASS; CLI `thispc-view 0.35.0`, exit code 0: PASS.

## Planowane wydania

### 0.36.0 — Network & Remote Locations ✅ ukończone, wydane (v0.36.0)
- SMB, SFTP, FTP, WebDAV via KIO where appropriate;
- saved remote locations;
- recent locations i reconnect tam, gdzie są użyteczne;
- KWallet integration dla poświadczeń tam, gdzie jest właściwa;
- discovery tylko tam, gdzie jest niezawodne; nigdy nie jest wymagane do
  ręcznego połączenia.
- poświadczenia obsługuje natywne KIO/KWallet; ThisPC nie zapisuje haseł;
- ręczny odbiór Stage 1–5: PASS; ścieżka z rzeczywistym wykrytym serwerem SMB: NOT TESTED z powodu braku wykrywalnych usług LAN;
- pełna regresja: 39 zestawów / 9075 asercji PASS; CLI `thispc-view 0.36.0`, kod wyjścia 0: PASS.

### 0.37.0 — Właściwości (Properties) ✅ ukończone, wydane (v0.37.0)
- Stage 1: model danych właściwości + General Info ✅ ukończone
- Stage 2: właściwości dysków / systemów plików ✅ ukończone
- Stage 3: uprawnienia POSIX + edytor ACL ✅ ukończone
- Stage 4: sumy kontrolne SHA-256 ✅ ukończone
- Stage 5: metadane KDE / KFileMetaData ✅ ukończone
- Stage 6: beztrybowe okna właściwości pliku/folderu/dysku (modeless UX polish) ✅ ukończone
- ręczny odbiór Stage 1–6: FULL MANUAL PASS.
- pełna regresja: 46 zestawów / 9690 asercji PASS; CLI `thispc-view 0.37.0`, kod wyjścia 0: PASS.

### 0.38.0 — Narzędzia pamięci masowej (Storage Tools) ✅ ukończone, wydane (v0.38.0)
- największe katalogi i pliki;
- skanowanie dysku w tle;
- widoki wykorzystania przestrzeni Top-N;
- narzędzia sum kontrolnych SHA-256 / SHA-1 / MD5;
- wykrywanie duplikatów;
- bezpieczny przegląd przed usunięciem lub przeniesieniem;
- interaktywna mapa zajętości (Treemap) z układem squarified, kanoniczną własnością fizyczną i zagłębianiem;
- naprawa cyklu życia i zamykania dialogów modeless.
- Stage 1: Rdzeń skanowania pamięci masowej (Storage Scan Core) ✅ ukończone
- Stage 2: Największe pliki i katalogi / Top-N ✅ ukończone
- Stage 3: Narzędzia skrótów (Hash Utilities) ✅ ukończone
- Stage 4: Wyszukiwarka duplikatów (Duplicate Finder) ✅ ukończone
- Stage 5: Bezpieczny przegląd i akcje (Safe Review & Actions) ✅ ukończone
- Stage 6: Mapa zajętości, szlif narzędzi pamięci masowej i naprawa cyklu życia ✅ ukończone
- Odbiór ręczny Stages 1–6: FULL MANUAL PASS.
- Pełna automatyczna regresja: 52 zestawy PASS; build, CLI `thispc-view 0.38.0`, kod wyjścia 0: PASS.

**Zasady globalne i bezpieczeństwo:**
- **Tylko lokalne ścieżki (Local Only):** rekurencyjne skanowanie pamięci masowej i wyszukiwanie duplikatów w 0.38 dotyczą wyłącznie lokalnych systemów plików. Zdalne zasoby KIO (SFTP, SMB, FTP, WebDAV) są bezwzględnie poza zakresem rekurencyjnego skanowania;
- **Brak podążania za symlinkami (No Symlink Follow):** domyślnie skaner nie podąża rekurencyjnie za dowiązaniami symbolicznymi. Symlink może być uwzględniony jako pojedynczy wpis, lecz jego cel nie jest rekurencyjnie skanowany (zapobieganie pętlom oraz wychodzeniu poza katalog bazowy skanu);
- **Świadomość hardlinków (Hardlink Aware):** dowiązania twarde są śledzone przez stabilną lokalną tożsamość (`st_dev` + `st_ino`). Pojedynczy fizyczny plik nie może być wielokrotnie wliczany do fizycznego zużycia miejsca, a hardlinki wskazujące na ten sam inode nie mogą być traktowane jako zwykłe duplikaty zawartości;
- **Granice systemów plików (Mount Boundaries):** domyślnie skan nie schodzi automatycznie na inne zamontowane systemy plików znajdujące się pod ścieżką początkową. Polityka opiera się o rzeczywistą tożsamość systemu plików (`st_dev`), a nie heurystykę ścieżek. Możliwość przyszłego jawnego włączenia („uwzględnij inne systemy plików”) pozostaje otwarta bez wymuszania jej w UI Stage 1;
- **Rozmiar logiczny vs. alokowany (Logical vs. Allocated Size):** rozróżnianie rozmiaru logicznego oraz rozmiaru alokowanego na dysku (size on disk / bloki) tam, gdzie system plików udostępnia te dane. Pliki rzadkie (sparse files) muszą być reprezentowane poprawnie, bez zakładania tożsamości rozmiaru logicznego z fizyczną alokacją;
- **Pliki ukryte (Hidden Files):** ukryte pliki i katalogi są domyślnie uwzględniane w obliczaniu całkowitej zajętości przestrzeni; ewentualne ukrycie ich w widoku prezentacji nie może zmieniać rzeczywistych sum;
- **Błędy i diagnostyka:** odmowa dostępu (permission denied), zniknięcie pliku w trakcie skanu, brak możliwości odczytu czy przejściowe błędy `stat` są normalnymi wynikami skanowania; należy je raportować w zagregowanych licznikach (pominięte, niedostępne, zniknięte, błędy) zamiast wyświetlania uciążliwych okien dialogowych dla każdego pliku;
- **Najpierw tylko do odczytu i brak automatycznego usuwania:** etapy 1–4 to wyłącznie analiza w trybie tylko do odczytu (read-only). Jakiekolwiek operacje na plikach są dopuszczalne dopiero w Stage 5 po jawnym przeglądzie użytkownika. Automatyczne usuwanie („usuń wszystkie duplikaty oprócz jednego”) bez świadomego wyboru jest zabronione.

**Stage 1 — Rdzeń skanowania pamięci masowej (Storage Scan Core):**
- asynchroniczny silnik rekurencyjnego skanowania lokalnego, zachowujący pełną responsywność interfejsu użytkownika;
- wyłącznie lokalne ścieżki i systemy plików; zdalne lokalizacje KIO (SFTP, SMB, FTP, WebDAV) są wyłączone z rekurencyjnego skanowania;
- raportowanie postępu, obsługa anulowania (Cancel), licznik przeskanowanych elementów oraz licznik przetworzonych bajtów tam, gdzie ma to sens;
- bezpieczeństwo symlinków: uwzględnienie wpisu bez przechodzenia do celu; ochrona przed pętlami i ucieczką poza korzeń skanu;
- obsługa hardlinków: stabilna identyfikacja (`st_dev` + `st_ino`) zapobiega zawyżaniu fizycznego zużycia miejsca;
- kontrola granic montowania: domyślne pozostawanie w obrębie początkowego systemu plików (`st_dev`);
- metryki rozmiaru: rozmiar logiczny obok alokacji dyskowej (uwzględnienie sparse files);
- pliki i foldery ukryte domyślnie wliczane do sumarycznej zajętości;
- zbiorcze raportowanie błędów i pominięć (pominięte, niedostępne, zniknięte, błędy) bez wyskakujących okienek;
- tryb wyłącznie do odczytu; brak jakichkolwiek mutacji na systemie plików.

**Stage 2 — Największe pliki i katalogi / Top-N (Largest Files & Directories / Top-N):**
- widoki największych plików oraz największych katalogów oparte bezpośrednio na danych z silnika Stage 1 (bez budowania osobnego mechanizmu skanowania);
- ranking Top-N, interaktywne sortowanie kolumn oraz przechodzenie w głąb katalogów (drill-down);
- akcja „Pokaż w folderze” oraz przejście do lokalizacji wybranego elementu;
- prezentacja rozmiaru logicznego vs. alokowanego tam, gdzie ma to zastosowanie;
- ścisła spójność danych z wynikami Storage Scan Core.

**Stage 3 — Narzędzia sum kontrolnych (Hash Utilities):**
- wspierane algorytmy: SHA-256 (domyślny), SHA-1 (zgodność / weryfikacja integralności), MD5 (zgodność / weryfikacja integralności);
- SHA-1 i MD5 wyraźnie oznaczone jako algorytmy wyłącznie do weryfikacji integralności/zgodności, nigdy jako bezpieczne skróty kryptograficzne;
- asynchroniczne obliczanie sum kontrolnych dla lokalnych zwykłych plików z paskiem postępu, możliwością anulowania oraz kopiowaniem wyniku do schowka;
- ponowne wykorzystanie (reuse) sprawdzonych wzorców z implementacji sum kontrolnych w oknie właściwości (0.37), o ile pozwala na to architektura, zapobiegając zbędnej duplikacji kodu.

**Stage 4 — Wyszukiwanie duplikatów (Duplicate Finder):**
- wieloetapowy potok wykrywania:
  1. wyłącznie lokalne zwykłe pliki (regular files);
  2. grupowanie kandydatów według dokładnego rozmiaru w bajtach;
  3. obliczanie skrótu tylko dla grup o identycznym rozmiarze (brak operacji I/O dla plików o unikalnym rozmiarze);
  4. potwierdzenie duplikatów przez porównanie sumy kontrolnej zawartości.
- nazwa pliku, rozszerzenie ani znaczniki czasu nigdy nie stanowią dowodu na duplikat zawartości;
- rozpoznawanie dowiązań twardych: pliki o tym samym inode są identyfikowane i nie są prezentowane jako duplikaty zawartości;
- minimalizacja obciążenia wejścia/wyjścia (I/O).

**Stage 5 — Bezpieczny przegląd i akcje (Safe Review & Actions):**
- obowiązkowy interaktywny przegląd przed jakąkolwiek mutacją: użytkownik świadomie zaznacza konkretne elementy; brak automatycznego kasowania „wszystkich prócz jednego”;
- bezpieczne operacje: Kosz (Trash), Przeniesienie (Move) oraz Pokaż w folderze (Show in folder);
- wykorzystanie istniejącej, bezpiecznej infrastruktury ThisPC: `FileActions`, `OperationManager`, Undo/Redo oraz zweryfikowanych ścieżek transferu i usuwania (brak surowego, bezpośredniego `unlink()`);
- wstępna ponowna walidacja (preflight revalidation) celów, chroniąca przed operacjami na plikach usuniętych lub podmienionych w międzyczasie.

**Stage 6 — Końcowy UX narzędzi pamięci masowej i regresja (Final Storage Tools UX & Regression):**
- dopracowanie spójności całego interfejsu, układu oraz parytetu między panelem głównym a Split View tam, gdzie funkcja jest dostępna;
- dopracowanie obsługi anulowania (Cancel) i płynności paska postępu;
- przejrzyste podsumowanie diagnostyki i błędów (error-summary UX);
- weryfikacja wydajności i stabilności zużycia pamięci przy operacjach na bardzo dużych strukturach katalogów;
- pełna automatyczna regresja testowa oraz manualna akceptacja w środowisku KDE/CachyOS;
- aktualizacja dokumentacji i przygotowanie wydania dopiero po pełnym przejściu testów (PASS);
- **Mapa zajętości (Treemap):** czysto pamięciowa wizualizacja Squarified Treemap z kanoniczną własnością fizyczną dla dowiązań twardych, przełączaniem metryki logicznej, zagłębianiem w poddrzewa, etykietami informacyjnymi i nawigacją klawiaturą;
- **Naprawa cyklu życia i zamykania dialogów modeless:** deterministyczne kaskadowe zamykanie dialogów i obsługa `done(r)` -> `close()`, zapewniające poprawne działanie `WA_DeleteOnClose` oraz naturalne zakończenie pętli zdarzeń Qt.


### 0.39.0 — Natywna integracja plików i bogate podglądy (Native File Integration & Rich Previews) ✅ ukończone, wydane (v0.39.0)

**Stage 1 — Uruchamianie plików lokalnych i integracja KIO (Local File Launch / KIO Integration)** ✅ ukończone (testy automatyczne i manualne PASS)
- Standardowy proces KDE Open/Execute przez `KIO::OpenUrlJob` (`setShowOpenOrExecuteDialog(true)`, `setRunExecutables(false)`);
- Lokalne pliki `.exe` i binarne respektują skojarzenia systemowe MIME (np. Wine/Bottles) bez błędu blokady `ERR_ACCESS_DENIED`;
- Prawidłowa semantyka adresów lokalnych `file://` oraz przekierowanie wpisów katalogów (`thispc:/` -> lokalny punkt montowania);
- Czysty deterministyczny komponent `LaunchUrlResolver` wymuszający ścisłe dopasowanie `drive.id`, leksykalną blokadę traversalu i zachowanie protokołów zdalnych;
- Pełna parzystość między panelem głównym, Split View i wynikami wyszukiwania;
- Bezpieczna polityka dla plików zdalnych; brak własnego launchera Wine, brak wywołań powłoki/QProcess, brak omijania zabezpieczeń.

**Stage 2 — Rdzeń podglądów i architektura miniatur (Preview Core / Thumbnail Architecture)** ✅ ukończone (testy automatyczne i manualne PASS):
- Dedykowany moduł koordynacji podglądów i miniatur (`src/previewcontroller.h` / `src/previewcontroller.cpp` lub `src/thumbnailcontroller.h` / `.cpp`);
- Asynchroniczne planowanie zadań przy użyciu natywnego stacku KDE (`KIO::PreviewJob` / twórcy miniatur);
- Priorytetyzacja widocznych elementów i śledzenie widoku paneli;
- Natychmiastowe anulowanie i tokeny generacji przy zmianie katalogu;
- Pamięć podręczna miniatur w pamięci RAM zintegrowana ze standardowym cache KDE;
- Bezpieczne zamykanie okna i obsługa wielu niezależnych instancji.

**Stage 3 — Miniatury obrazów i wideo (Image & Video Thumbnails)** ✅ ukończone (testy automatyczne i manualne PASS):
- Podglądy obrazów: PNG, JPEG, WebP oraz formaty obsługiwane przez systemowe wtyczki;
- Klatki miniatur wideo przez infrastrukturę podglądu KDE (`ffmpegthumbs` / systemowe thumbnailery);
- Respektowanie współczynnika DPI ekranu i rozmiaru ikon bez blokowania wątku interfejsu;
- Płynny fallback do standardowej ikony MIME w przypadku braku miniatury.

**Stage 4 — Ikony plików wykonywalnych i binariów (Executable / File Icon Previews)** ✅ ukończone (testy automatyczne i manualne PASS):
- Pobieranie natywnych ikon dla plików Windows `.exe` i binariów PE zgodnie z prezentacją Dolphina;
- Wykorzystanie istniejącej infrastruktury wtyczek thumbnailerów KDE;
- Deterministyczny łańcuch fallbacku: miniatura -> natywna ikona systemowa -> ikona MIME.

**Stage 5 — Podgląd zawartości folderów (Folder Content Previews)** ✅ ukończone (testy automatyczne i manualne PASS):
- Miniaturowy podgląd zawartości folderów (np. foldery ze zdjęciami) w stylu Dolphina;
- Integracja ze standardowym systemem podglądu katalogów KDE bez własnego ad-hoc skanowania dysku;
- Fallback do standardowej ikony katalogu w przypadku braku podglądu lub pustego folderu.

**Stage 6 — Integracja z widokami i UX (View Integration & UX)** ✅ ukończone (testy automatyczne i manualne PASS):
- Jednolita prezentacja podglądów w panelu głównym, panelu podzielonym i wynikach wyszukiwania;
- Obsługa we wszystkich trybach: Ikony, Lista, Szczegóły, Kafelki/Kompaktowy;
- Opcja widoku: `Widok -> Pokaż podglądy` (`View -> Show Previews`);
- Spójność z istniejącym systemem ustawień widoku per-folder i modelem zapisu.

**Stage 7 — Wydajność, cykl życia i szlif końcowy (Performance / Lifetime / Final Polish)** ✅ ukończone (testy automatyczne i manualne PASS):
- Całkowity brak blokowania wątku GUI i synchronicznego I/O;
- Kontrola zużycia pamięci; brak generowania miniatur dla tysięcy elementów naraz;
- Podglądy włączone dla systemów lokalnych, domyślnie wyłączone dla lokalizacji zdalnych (`sftp:`, `smb:`, `fish:`);
- Kompletny zestaw testów automatycznych i manualna akceptacja w środowisku KDE Plasma 6.


### 0.40.0 — Rozszerzone właściwości — dalszy rozwój (Advanced Properties Follow-up)
- atrybuty rozszerzone (`xattr` / podgląd i edycja tam, gdzie są wspierane);
- numeryczna edycja uprawnień POSIX (`0755`) zsynchronizowana z istniejącym interfejsem;
- semantyka ukrywania zależna od backendu (kropka w nazwie na uniksach vs. prawdziwe metadane/flagi atrybutów).

### 0.41.0 — Advanced Search
- type/name/extension/date/size filters;
- files-only/folders-only;
- saved searches;
- Baloo acceleration when available, current search fallback otherwise;
- szybki filtr bieżącego folderu jako lekkie rozszerzenie in-place, jeśli jego
  semantyka pozostanie spójna z wyszukiwaniem.

### 0.42.0 — Administrator fallback for failed operations
When a normal operation receives permission denied, offer a targeted `admin://` retry instead of requiring an entire window to run elevated.

### 0.43.0 — Transfer Queue & Control
- serial vs parallel;
- concurrency limit;
- priorities/order;
- queue-wide pause/resume/control;
- optional bandwidth limit wyłącznie dla backendów, które realnie mogą go
  zapewnić.

### 0.44.0 — Notifications + Operation History
- Plasma notification for long/background completions;
- recent operation log with source/destination/result;
- retry where meaningful and safe;
- „Show in folder” po zakończeniu tam, gdzie ma sens.

### 0.45.0 — Extensions / Service Actions
- preferować wykorzystanie KDE Service Actions i prostego extension contract
  zamiast budowania pełnego własnego plugin ecosystemu bez konkretnej potrzeby;
- pozwolić dodawać nowe context actions i integracje bez edycji core window source.

### 0.46.0 — Advanced Split Sync
- opcjonalne recursive compare/sync;
- dry-run i plan preview przed wykonaniem;
- ignore patterns;
- jawne conflict policies;
- domyślnie nadal zero-delete;
- mirror/delete dopiero jako jawny, wyraźnie niebezpieczny tryb z osobnym
  potwierdzeniem;
- zachować model safety, preflight i revalidation wypracowany w 0.31.

### 0.47.0 — Image Printing / Print Pictures workflow
- zastąpić i rozszerzyć obecną pojedynczą ścieżkę `Print` własnym, przewidywalnym
  workflow drukowania obrazów; obecnie `Print` jest wyłączone przy zaznaczeniu
  wielu obrazów i samo odblokowanie starej akcji nie jest rozwiązaniem;
- obsłużyć kilka lub kilkanaście zaznaczonych plików graficznych w jednym
  zadaniu, z bezpiecznym dopuszczaniem tylko wspieranych formatów i czytelną
  obsługą mixed selection;
- zapewnić ergonomię zbliżoną funkcjonalnie do Windows 11 „Print Pictures”, bez
  kopiowania wyglądu 1:1: duży podgląd bieżącej strony/obrazu, liczba stron oraz
  nawigacja po stronach zestawu;
- dodać chooser presetów układu, np. cała strona, 13×18, 10×15 oraz wiele zdjęć
  na stronie; opcjonalny contact sheet traktować jako naturalne rozszerzenie;
- udostępnić wybór drukarki, rozmiaru papieru, liczby kopii i przełącznik
  fit-to-frame/crop, a jakość/DPI oraz typ papieru/nośnika pokazywać tylko wtedy,
  gdy backend lub sterownik faktycznie zgłasza takie możliwości;
- korzystać z Qt Print Support oraz wykrywania capabilities przez CUPS/IPP tam,
  gdzie jest to dostępne; nie hardcode'ować opcji niewspieranych przez drukarkę;
- utrzymać preview i layout engine na tyle niezależne od systemowego modalnego
  print dialogu, aby UX oraz wynik rozmieszczenia były przewidywalne;
- zapewnić identyczne działanie dla Primary i Split View oraz testy layoutu,
  paginacji, capability fallback, mixed selection i wieloplikowego jobu.

## Backlog bez przypisanej wersji

### Ark → ThisPC Drag & Drop interoperability — audyt i plan
- przyjmować external Drag & Drop z Ark/KDE archive views do zwykłego folderu
  w ThisPC dla plików i folderów, zarówno single-, jak i multi-selection;
- najpierw zbadać rzeczywisty kontrakt MIME/URI/temporary extraction Ark/KIO;
  nie zakładać, że Ark zawsze wystawia lokalne URL-e `file://`, ani nie obiecywać
  implementacji opartej na prostym `copy` bez tego audytu;
- domyślnie stosować semantykę Copy; Move udostępniać wyłącznie wtedy, gdy
  źródło i backend rzeczywiście je wspierają, a operacja jest bezpieczna;
- obsłużyć konflikty oraz Undo/Redo tylko tam, gdzie ich gwarancje można realnie
  zachować dla kontraktu dostarczonego przez źródło;
- nie blokować DnD wyłącznie dlatego, że źródłem jest zewnętrzna aplikacja;
- zapewnić parity Primary/Split i wszystkich trybów widoku;
- testować parser/negocjację przez synthetic `QMimeData`, a rzeczywistą
  interoperacyjność przez ręczny Ark smoke test dla plików, folderów oraz wielu
  zaznaczonych elementów.

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

### Smaller features — wersje do ustalenia
- Ctrl+Shift+T — reopen closed tab;
- opcjonalne pinned tabs;
- Copy Path / Copy Filename;
- Recent Files jako rozszerzenie Recent Locations;
- lepszy Trash z Original location i Deletion date;
- video thumbnails przez infrastrukturę KDE/KIO thumbnail i `ffmpegthumbs`,
  asynchronicznie, z cache i fallbackiem, bez blokowania UI oraz z ostrożnym
  zachowaniem dla remote KIO;
- status bar details (liczba elementów/zaznaczonych, suma rozmiaru zaznaczenia,
  wolne miejsce), jeśli candidate nie wejdzie do 0.34.0.

## Pomysły do rozważenia w przyszłości / Future ideas

Punkty w tej sekcji nie są zobowiązaniem do implementacji ani przypisaniem do
wersji. Przy każdym kolejnym przeglądzie roadmapy należy ponownie ocenić ich
sens i usunąć je, jeśli przestaną być użyteczne.

- opcjonalny appearance candidate: regulacja siły przygaszenia widocznych
  hidden items (obecny domyślny poziom: 0.40). Na razie bez decyzji, czy takie
  ustawienie jest w ogóle potrzebne; jeśli kiedyś powstanie, preferować prostą
  preferencję wyglądu lub kilka presetów zamiast suwaka w menu Widok.
- opcjonalne dźwięki akcji/zdarzeń: dyskretne sygnały dźwiękowe dla długich zadań, błędów lub zakończenia operacji (wyłącznie niezobowiązujący kandydat w przyszłości).
- Kandydat integracji systemowej: możliwość ustawienia ThisPC jako domyślnego programu/menedżera do otwierania katalogów (inode/directory) przez standardowe mechanizmy XDG/KDE, z wykrywaniem obecnego handlera i możliwością bezpiecznego przywrócenia poprzedniego.
- Kandydat wizualizacji pamięci masowej: interaktywny widok mapy drzewa (treemap) do analizy zajętości dysku (odłożony poza podstawowy zakres 0.38 jako nieblokujący kandydat na przyszłość).

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
- long-running session i resource-leak soak tests;
- końcowy accessibility/keyboard pass;
- high-DPI i multi-monitor polish;
- regression tests for file-operation safety.
