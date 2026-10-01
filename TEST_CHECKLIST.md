# TEST_CHECKLIST.md

## Build sanity
- [ ] clean configure succeeds
- [ ] `thispc` KIO module builds
- [ ] `thispc-view` builds
- [ ] no new compiler errors
- [ ] review new warnings rather than ignoring them blindly

## Core regression smoke test
- [ ] `thispc-view` starts
- [ ] Ten komputer opens
- [ ] home folders open
- [ ] drive entries open
- [ ] back/forward/up work
- [ ] tabs open/close/reorder
- [ ] Split View opens/closes and both panes navigate
- [ ] icons/list/details still render
- [ ] sorting works
- [ ] search still works
- [ ] operation popup opens and remains positioned mostly to the right
- [ ] starting an operation automatically opens the detailed operation window
- [ ] one and multiple simultaneous operations size the detailed window correctly
- [ ] current file, bytes, prominent percentage, speed, average, ETA and graph update
- [ ] Mniej/Szczegóły and Cancel respond during a fast transfer
- [ ] completed operations disappear from the detailed window but remain in compact history
- [ ] the detailed window closes after the final active operation
- [ ] version label is visible on dark theme

## File operations
Use disposable test files only.
- [ ] create folder
- [ ] create file
- [ ] rename
- [ ] copy
- [ ] move
- [ ] Trash
- [ ] Undo rename
- [ ] Redo rename
- [ ] Undo Trash restore
- [ ] Undo/Redo buttons enable/disable correctly

## Permissions
Only test on disposable files.
- [ ] normal POSIX permission edit on a Linux filesystem
- [ ] read-back verification works
- [ ] no blanket NTFS-disable behavior returns
- [ ] admin fallback remains targeted


## Sidebar & Split View regression

- [ ] both Split View panes navigate independently
- [ ] shared toolbar/sidebar actions target the active pane
- [ ] F6 switches the active pane without losing pane state
- [ ] This PC / `thispc:/` cards work in both panes
- [ ] Search query, filters, results and Stop remain independent per pane
- [ ] sidebar Drag & Drop works after scrolling
- [ ] Quick Access reorder works after scrolling
- [ ] sidebar width persists after restart
- [ ] long sidebar labels use right-side ellipsis and show the full tooltip
- [ ] no horizontal sidebar scrollbar appears
- [ ] Ustawić nierówny podział paneli; krótkie/głębokie ścieżki i bardzo długa nazwa katalogu po obu stronach nie przesuwają separatora.
- [ ] Przy długich ścieżkach przeciągnąć separator, zmienić rozmiar okna, przełączyć aktywny panel i Ten komputer; wybrany podział pozostaje zachowany.
- [ ] Przewijanie segmentów po lewej, wielokropek po prawej, podpowiedzi i Ctrl+L zapewniają dostęp do pełnego adresu.
- [ ] Wyłączyć/włączyć Split View i ponownie uruchomić aplikację; poprzedni podział wraca również po zamknięciu z ukrytym prawym panelem.

## Automated regression

- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` exits with code 0
- [ ] all focused suites pass
- [ ] `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites split_layout panes search` przechodzi regresję szerokości paneli.
- [x] `git diff --check` is clean

## Final release verification — 0.36.0 (2026-10-01)

- [x] `./scripts/build.sh` — PASS (`thispc` and `thispc-view`).
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 39 suites / 9075 assertions PASS.
- [x] `tests/version-cli.py` — 6/6 assertions PASS.
- [x] Built `thispc-view --version` prints `thispc-view 0.36.0` and exits 0.
- [x] `git diff --check` — PASS.
- [x] Stages 1–5 manually accepted.
- [ ] Live discovered SMB server path — NOT TESTED; no discoverable LAN services available.

## Final release verification — 0.35.0 (2026-09-30)

- [x] `./scripts/build.sh` — PASS (`thispc` and `thispc-view`).
- [x] Focused `drive_home`, `solid_monitor`, `device_mount`, `device_removal` — 259 assertions PASS.
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 35 suites / 8562 assertions PASS.
- [x] `tests/version-cli.py` — 6/6 assertions PASS.
- [x] Built `thispc-view --version` prints `thispc-view 0.35.0` and exits 0.
- [x] `git diff --check` — PASS.
- [x] Stages 1–3 manually verified on real USB hardware:
  - dynamic hotplug detection without restart/F5,
  - unmounted removable volume discovery on `thispc:/`,
  - mount on click/Enter with auto-navigation into directory,
  - sidebar displays mounted-only volumes,
  - filesystem-only Unmount preserves device power and block device,
  - Safely Remove unmounts all member partitions then powers off physical drive,
  - Safely Remove works for already-unmounted volumes,
  - Primary and Split panes cleanly exit dead mountpoints back to `thispc:/`,
  - no periodic polling and no "Odświeżanie…" flicker.
- [ ] Standard system install requires the user's interactive `sudo` password.

## Final release verification — 0.34.0 (2026-09-28)

- [x] `./scripts/build.sh` — PASS (`thispc` and `thispc-view`).
- [x] Focused pane/view/location/keyboard smoke plus Split layout — PASS.
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 32 suites / 8317 assertions PASS.
- [x] `tests/version-cli.py` — 6/6 assertions PASS.
- [x] Built and isolated-installed `thispc-view --version` prints `thispc-view 0.34.0` and exits 0.
- [x] Isolated `DESTDIR` install smoke — PASS.
- [x] `git diff --check` — PASS.
- [x] Stages 2–7 and final Split View visual parity polish manually accepted.
- [ ] Standard system install requires the user's interactive `sudo` password.
- [x] Final manual release smoke accepted; dwa znane cosmetic follow-upy są nieblokujące.

## Final release verification — 0.33.0 (2026-09-27)

- [x] `./scripts/build.sh` — PASS (`thispc` and `thispc-view`).
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 31 suites / 7852 assertions PASS.
- [x] Version consistency assertions expect `v0.33.0` and pass.
- [x] New-module header/implementation build and include-cycle audit pass.
- [x] Isolated `DESTDIR` install and offscreen launch smoke — PASS.
- [x] `git diff --check` — PASS.
- [x] Stage 1–8 and inline rename fix manually accepted before release prep.
- [ ] Final manual release smoke accepted.

## Final release verification — 0.32.0 (2026-09-25)

- [x] `./scripts/build.sh` — PASS (`thispc` and `thispc-view`).
- [x] Focused `panes tabs search selection_menu split_layout view_settings preview quick_look split_compare actions` — 10 suites / 1738 assertions PASS.
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 26 suites / 6662 assertions PASS.
- [x] Version consistency assertions expect `v0.32.0` and pass.
- [x] `git diff --check` — PASS.
- [x] All 0.32 stages manually accepted; Stage 9/common `BrowserPane` deferred to 0.33.

## Last verified automated baseline — 2026-09-18

- [x] `git diff --check` — PASS.
- [x] `cmake --build build -j2` — PASS.
- [x] `split_layout` — 3 consecutive runs, 99 assertions each.
- [x] `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — PASS, all suites.

The split_layout test verifies that the destination path
scrolls to its maximum position and exposes the right edge
of the final path segment.

A segment longer than the viewport is not required to fit
entirely inside the viewport.

0.26.0 manual KDE/CachyOS acceptance confirmed by the user on 2026-09-18.

## 0.26.0 Stage 4 — metadane audio/wideo i EXIF

- [ ] Audio: wybrać lokalny MP3/FLAC/WAV; panel pokazuje dostępne tagi, czas, bitrate,
  częstotliwość próbkowania i kanały, ale niczego nie odtwarza.
- [ ] Wideo: wybrać lokalny MP4/M4V obsługiwany przez TagLib; panel pokazuje bezpiecznie dostępne metadane,
  a uszkodzony plik daje komunikat błędu bez zawieszenia interfejsu.
- [ ] EXIF: wybrać lokalny JPEG ze zdjęcia; obraz nadal jest widoczny, a pod nim pojawiają
  się tylko dostępne pola aparatu, daty, ekspozycji, ISO i ogniskowej.
- [ ] Szybko zmieniać wybór między dużym plikiem multimedialnym, zdjęciem i tekstem;
  spóźniony wynik nie zastępuje bieżącego podglądu.
- [ ] Ukryć panel Alt+P podczas analizy i ponownie go pokazać; brak starego wyniku i awarii.
- [ ] Sprawdzić plik większy niż 4 GiB (może być rzadki test ręczny); pojawia się limit,
  bez próby pełnej analizy.
- [x] Focused: `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites preview`.
- [x] Pełna regresja: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all`.
- [x] `git diff --check` — PASS; numer wersji 0.26.0 zaktualizowany, commit/tag/instalacja nadal oczekują.

## 0.26.0 Stage 5 — zawartość archiwów

Diagnostyka Stage 5: **PASS, 101 asercji**. Następnie użytkownik uruchomił
focused preview oraz pełną regresję w zwykłej sesji KDE (oba exit code 0).
Po poprawce renderowania PDF: **20 zestawów PASS**, w tym **106 asercji Preview Pane**
i **99 asercji Split Layout**. Użytkownik potwierdził końcową ręczną akceptację
0.26.0; szczegółowych przypadków niezweryfikowanych osobno nie odhaczamy.

- [ ] ZIP: lista pokazuje katalogi i pliki w kolejności archiwum, nazwy UTF-8 i rozmiary.
- [ ] 7z, tar i tar.gz: lista działa dla formatów obsługiwanych przez istniejący backend.
- [ ] Puste, uszkodzone, zaszyfrowane i niewspierane archiwum: jasny komunikat bez dialogu Ark.
- [ ] Archiwum z niebezpieczną ścieżką, linkiem lub nadmierną liczbą wpisów jest odrzucane.
- [ ] Podgląd nie rozpakowuje, nie uruchamia i nie zapisuje żadnej zawartości archiwum.
- [ ] Szybko zmieniać wybór archiwum/tekst/obraz; spóźniony manifest nie zastępuje wyboru.
- [ ] Alt+P podczas analizy, ponowne pokazanie i resize: brak starego wyniku i awarii.
- [ ] Potwierdzić brak regresji PDF, obraz/EXIF, tekst, multimedia, folder summary i Split View.
- [x] Focused: `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites preview`.
- [x] Pełna regresja: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all`.
- [x] `git diff --check` — PASS; numer wersji 0.26.0 zaktualizowany, commit/tag/instalacja nadal oczekują.

## 0.26.0 — ponowna akceptacja PDF i EXIF

- [ ] Otworzyć lokalne PDF-y, które wcześniej miały szare lub częściowo szare strony;
  puste obszary pierwszej strony są białe, a treść pozostaje kompletna.
- [ ] Sprawdzić PDF tekstowy, skan oraz dokument z nietypowym rozmiarem/kadrem strony;
  zgłosić osobno ewentualne braki treści (ograniczenie QtPdf), nie tylko kolor tła.
- [ ] EXIF: użyć zdjęcia bezpośrednio z aparatu lub telefonu. EXIF to metadane zdjęcia,
  np. producent/model aparatu, data wykonania, ekspozycja, ISO i ogniskowa. Brak sekcji
  EXIF przy zwykłym JPEG-u bez takich danych nie jest błędem.
- [x] Użytkownik potwierdził końcową ręczną akceptację podglądu PDF i EXIF (2026-09-18).

## 0.25.0 Stage 2 — archiwa

- [ ] Wykonać listę manual KDE z docs/ARCHIVE_STAGE2.md, wyłącznie na danych jednorazowych.
- [ ] Focused: TMPDIR=/tmp python3 tests/run-pane-actions.py --suites archive archive_jobs archive_menu.
- [ ] Pełna regresja: TMPDIR=/tmp python3 tests/run-pane-actions.py --all.
- [ ] git diff --check; bez commita/taga przed manualną akceptacją.

## 0.27.0 — Quick Look on Space

- [x] Wszystkie 9 przypadków z `docs/QUICK_LOOK_STAGE1.md` potwierdzone ręcznie przez użytkownika na KDE/CachyOS (2026-09-19).
- [x] Space otwiera/zamyka duży podgląd pojedynczego zaznaczenia w lewym i prawym panelu; Esc zamyka.
- [x] Strzałki zmieniają zaznaczenie pod otwartym Quick Look, a fokus pozostaje w aktywnym widoku plików.
- [x] Space wpisuje znak w Search, Ctrl+L, zmianie nazwy i dialogach zamiast otwierać Quick Look.
- [x] Szybka zmiana zaznaczenia, zmiana aktywnego panelu i zamknięcie podczas ładowania nie pokazują starego wyniku.
- [x] Alt+P Preview Pane działa niezależnie przed, podczas i po Quick Look.
- [x] Focused: `TMPDIR=/tmp python3 tests/run-pane-actions.py --suites quick_look preview panes split_layout`.
- [x] Full: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 22 suites / 3598 assertions PASS.

## 0.28.0 — Batch Rename

> Sekcje etapowe poniżej są historycznym zapisem kolejnych wycinków. Bieżące
> rozstrzygnięcie wydania znajduje się w sekcji „Audyt końcowy 0.28.0”.

- [x] Stage 1: 8/8 testów ręcznych potwierdzone przez użytkownika.
- [x] Focused Stage 2: `batch_rename panes actions` — 50 + 287 + 157 asercji PASS.
- [ ] Wykonać ręczną checklistę Stage 2 z `docs/BATCH_RENAME_STAGE2.md` na jednorazowych danych.
- [x] Full: `TMPDIR=/tmp python3 tests/run-pane-actions.py --all` — 23 zestawy / 3666 asercji PASS.
- [x] `git diff --check` i build; bez zmiany wersji, commita, taga, ZIP, instalacji ani publikacji.

## 0.28.0 — Batch Rename Stage 3A

- [x] Patch Stage 3A zastosowany; build PASS.
- [x] Focused `batch_rename panes actions`: 85 + 287 + 157 = 529 PASS.
- [x] Ręczny test łańcucha `1 → 41 → 441` i zachowania obu zawartości PASS.
- [ ] Pełna regresja po Stage 3A.
- [ ] Pozostała ręczna checklista Stage 3A i pełna Stage 2 (tylko błędny regex ręcznie potwierdzony).

## Stage 3B.1 — atomowy swap dwóch nazw, patch do weryfikacji

- [ ] Zastosować Stage 3B.1 patch na bazie Stage 3A i `git diff --check`.
- [ ] `./scripts/build.sh` i focused `batch_rename panes actions`.
- [ ] Pełna regresja `tests/run-pane-actions.py --all`.
- [ ] Ręczne testy w `docs/BATCH_RENAME_STAGE3.md`: pliki, katalogi, symlinki,
      odrzucenie ostrzeżenia, race po preview, Split View i brak Ctrl+Z dla swapu.
- [ ] Cykle 3+, trwałe recovery i wspólne Undo/Redo — **niewdrożone**;
      nie wydawać 0.28.0.


## 0.28.0 — Stage 3B.2: cykle 3+ (testy automatyczne PASS, ręczny cykl 3 PASS)

- [x] Zastosować przyrostowy patch Stage 3B.2 na repo z Stage 3B.1; nie
      nadpisywać lokalnych zmian użytkownika.
- [x] `./scripts/build.sh` — konfiguracja i kompilacja Qt6/KF6.
- [x] Focused: `env TMPDIR=/tmp python3 tests/run-pane-actions.py --suites batch_rename panes actions`.
- [x] Pełna regresja: `env TMPDIR=/tmp python3 tests/run-pane-actions.py --all`.
- [x] `git diff --check`.
- [ ] Pełna ręczna checklista `docs/BATCH_RENAME_STAGE3.md` na kopiach w KDE;
      test cyklu 3 plików z zawartością potwierdzono, pozostałych nie.
- [ ] Pełna ręczna akceptacja Stage 2 (dotąd potwierdzono tylko wybrane przypadki).
- [ ] Trwały journal/recovery i wspólne Undo/Redo nadal niewdrożone;
      nie uznawać 0.28.0 za gotowe do wydania.

## 0.28.0 — Stage 3C.1: journal diagnostyczny (patch do weryfikacji)

- [ ] Kompilacja, focused `batch_rename panes actions`, pełna regresja, `git diff --check`.
- [ ] Testy dziennika: sygnatura source/inode/device, checkpoint i cleanup, przerwany
      obiekt pozostawia plik, nierozwiązany manifest blokuje kolejną próbę.
- [ ] KDE: cykl 3 plików na kopiach, poprawna zawartość i brak journalu po sukcesie.
- [ ] Nie deklarować automatycznego recovery ani zbiorczego Undo/Redo: nadal brak.


## 0.28.0 Stage 3C.2A — isolated atomic pair Undo/Redo (basic KDE acceptance PASS)

- [x] Two disposable sibling files `uv` / `vu`: complete one exchange; Ctrl+Z
      restores both payloads in one step and Ctrl+Y exchanges them again.
- [x] After Undo, replace one disposable inode; Redo refuses with zero changes
      (manual KDE confirmation: original inode retained as `.original-1-backup`).
- [ ] 3+ cycle retains crash-inspection journal and no Undo; ordinary acyclic
      batch retains per-file KIO Undo (do not claim whole-batch Undo).
- [ ] Split View, Quick Look, normal file actions and full regression PASS.


## 0.28.0 Stage 3C.2B.1 — pojedynczy cykl 3+ (oczekuje na akceptację)

- [ ] Build, focused `batch_rename panes actions`, full `--all` i `git diff --check`.
- [ ] Testy: pełny snapshot inode+metadata, journal przy Undo/Redo, zawartość
      trzech plików po Undo i Redo, blokada przy istniejącym dzienniku.
- [ ] KDE: jedno Ctrl+Z i jedno Ctrl+Y dla samodzielnego cyklu trzech plików;
      sprawdzenie treści oraz braku pozostawionego `cycle-*.json`.
- [ ] KDE: po Undo podmień jeden testowy inode, sprawdź odmowę Redo bez zmian.
- [ ] Upewnij się, że łańcuchy i mieszane partie NIE reklamują wspólnego Undo.
- [ ] Nadal brak automatycznego recovery i atomowości całego cyklu; nie wydawać 0.28.0.


## Stage 3C.2B.2A — czyste lokalne łańcuchy (patch do weryfikacji)

- [ ] `./scripts/build.sh` — PASS wymagany na KDE/CachyOS.
- [ ] Focused `batch_rename panes actions`, full `--all`, `git diff --check`.
- [ ] Testy automatyczne: dwie niezależne grupy w jednym Undo/Redo, preflight
      całej mapy, fault injection po pierwszym kroku i poprawny rollback,
      blokada nieznanego inode oraz outsiders w pustym źródle.
- [ ] KDE: `1,41,2,42`, prefiks `4`; jedno Ctrl+Z i Ctrl+Y dla czterech plików.
- [ ] Odrzucenie dialogu nie zmienia plików; błąd/anulowanie zwykłego KIO
      nie tworzy historii grupowej i zachowuje wpisy ukończonych operacji.
- [ ] Samodzielny swap, cykl 3+, obie strony Split View, Quick Look: bez regresji.
- [ ] NIE deklarować wspólnego Undo/Redo dla partii łączących różne typy,
      automatycznego crash recovery ani atomowości wieloetapowej partii.

## 0.28.0 — odmowa Redo po zmianie inode: trwały komunikat UI

- [x] Kontroler: Undo → podmiana inode → Redo odmawia bez zmian, zachowuje
      aktywne Redo i po bezpiecznym przywróceniu oczekiwanego inode pozwala ponowić.
- [x] Integracja: `renderDirectoryItems` i asynchroniczny refresh z właściwością
      joba nie nadpisują komunikatu odmowy; zwykły refresh nie zmienia zachowania.
- [x] Build i focused `batch_rename search actions`: 246 + 122 + 157 = 525 PASS.
- [x] Full `--all`: 23 zestawy / 3865 asercji PASS.

## Audyt końcowy 0.28.0 — stan po ręcznych testach Stage 2/3

- [x] Stage 2 punkty 1–8 i 10 z `docs/BATCH_RENAME_STAGE2.md`: PASS. Obejmuje
      kolizje i race, mieszany no-op (1 zmiana + 3 pominięcia) z Undo/Redo,
      preview w obu panelach Split View i trzech trybach oraz regresje Rename,
      Trash, Quick Look i Preview Pane.
- [ ] Stage 2 punkt 9: anulowanie **w trakcie** większej niekwalifikującej się
      partii KIO z osobnymi wpisami Undo — nie wykonano. Nie oznaczać PASS.
      Zwykłe kwalifikujące się lokalne partie mają celowo brak Cancel w trakcie,
      journalowany nieatomowy replay i jedno wspólne Undo po pełnym sukcesie.
- [x] Anulowanie przed startem (odrzucenie ostrzeżenia): PASS, zero zmian.
- [x] Stage 3: swap oraz cykl 3 — wykonanie, jedno Undo i jedno Redo PASS.
- [x] Stage 3: dwa niezależne łańcuchy — wspólne Undo/Redo PASS.
- [x] Stage 3: odmowa Redo po podmianie inode bez zmian; komunikat pozostał
      widoczny około 12 s.
- [x] Stage 3: świeża dwuelementowa partia journalowana — wykonanie, jedno Undo
      i jedno Redo zachowały nazwy i zawartości; po każdym kroku brak
      `linear-*.json`. UI reagowało podczas krótkiej operacji; to ograniczona
      obserwacja, nie dowód responsywności dla wszystkich czasów wykonania.
- [x] Stage 3: odmowa całej operacji przed pierwszą zmianą po utworzeniu obcego
      `a.txt` (inode 13093128) — PASS. Jedno Ctrl+Z pokazało komunikat o zmianie
      plików/inode; `a.txt` zachował obcą zawartość i inode, `b.txt` nie powstał,
      `TEST_a.txt` (`PIERWSZY`, inode 13092581) i `TEST_b.txt` (`DRUGI`, inode
      13092582) pozostały bez zmian; brak `linear-*.json`.
- [x] Stage 3: aktywny lewy panel Split View — wykonanie i Undo/Redo PASS.
- [x] Stage 3: dwa ręczne przypadki race oraz cancel przed startem PASS.
- [x] Bieżąca baza (2026-09-19): `./scripts/build.sh` PASS; focused
      `batch_rename panes actions` — 268 + 287 + 157 = 712 asercji PASS;
      pełna regresja `--all` — 23 zestawy / 3887 asercji PASS;
      `git diff --check` PASS.
- [x] Polityka local-linear rozstrzygnięta w 3C.2: produkcyjne v2 recovery
      dokańcza zapisany kierunek; pozostałe klasy nadal blokują/manual-only.
- [ ] Jeżeli 0.28.0 ma oficjalnie obejmować niekwalifikujące się/zdalne plany
      KIO, wykonać kontrolowany test ich cancel w trakcie na jednorazowych
      danych. Dla lokalnego zakresu wydania test jest opcjonalny i nie należy
      próbować wymuszać go na szybkiej partii produkcyjnych plików.
- [ ] Różnice wizualne trybów widoku lewego/prawego panelu Split View zapisać
      jako osobne przyszłe zadanie; nie rozszerzają zakresu 0.28.0 bez wykazanej
      regresji funkcjonalnej.

## Crash recovery Stage 3C.1 — izolowane UI/black-box

- [x] Kierunkowe, trwałe statusy checking/recovering/success/conflict dla Undo/Redo.
- [x] Prawdziwy testowy build aplikacji: startup recovery obu kierunków i drugi
      restart; dokładne nazwy, treści oraz dev+ino.
- [x] Konflikt: brak zmian, obcy inode i treść zachowane, journal zachowany,
      mutacje nadal zablokowane.
- [x] Spowolniony worker nie blokuje głównego event loop; czysty startup PASS.
- [x] Produkcyjny `thispc-view` nie wykonuje automatycznie v2 Undo/Redo.
- [x] Focused Batch Rename: 1032/1032 PASS; pełne `--all`: PASS;
      produkcyjny build, testowy build i `git diff --check`: PASS.
- [x] 3C.2: kwalifikujące się local-linear Undo/Redo używa produkcyjnego v2;
      startup dokańcza dokładnie jeden poprawny v2 forward/undo/redo (polityka B).
- [x] Procesowa macierz 12 punktów SIGKILL × Undo/Redo przechodzi przez
      `UndoController::undo()`/`redo()` w prywatnym XDG; drugi restart idempotentny.
- [x] V1/corrupt/unknown/multiple/temp/live-lock pozostają fail-closed/manual-only;
      konflikt zachowuje journal i obcy inode, a mutacje pozostają zablokowane.
- [x] 3C.2/Etap 3 PASS w zakresie local-linear: build PASS; focused
      `batch_rename panes actions` 1476 PASS; pełne `--all` 23 zestawy / 4651
      asercji PASS; `git diff --check` PASS.
- [ ] Brak gwarancji power-loss bez disposable VM/FS-image; swapy/cykle/KIO
      fallback i Etap 4 poza zakresem. Wersja 0.27.0, release nadal wstrzymany.

## 0.28.0 — Etap 4A: crash recovery izolowanego swapu

- [x] Audyt istniejącego Execute/Undo/Redo, v2 i kolejności blokad; zamrożony
      osobny manifest `kind=swap` oraz deterministyczne mapy before/after.
- [x] 4A.1B: izolowany testowy parser/silnik v2 swapu — procesowe
      Execute/Undo/Redo, drugi SIGKILL podczas recovery, kolejne restarty,
      concurrency/lock lifecycle, fsync journal/data/cleanup, ENOSYS/
      EOPNOTSUPP, konflikty, podmiana katalogu, TOCTOU oraz ścisły validator
      plans/states/digest PASS. Nie oznacza aktywacji produkcyjnej.
- [ ] Worker dla swap Execute oraz Undo/Redo; kierunkowe trwałe statusy i gate.
- [ ] Black-box Execute/Undo/Redo, drugi restart, dwie instancje i GUI responsive.
- [ ] Produkcyjna allowlista startup recovery dla `kind=swap`.
- [ ] **4A NIEZALICZONY**; swapy nadal nie są automatycznie odzyskiwane.
- [x] Bieżąca weryfikacja części 4A.1: build PASS; focused `batch_rename panes
      actions` 1272 + 287 + 157 = 1716 asercji PASS; pełne `--all` 23 zestawy /
      4891 asercji PASS; `git diff --check` PASS; wersja nadal 0.27.0.
- [x] Weryfikacja 4A.1B (2026-09-20): `./scripts/build.sh` PASS; focused
      `batch_rename panes actions` 1757 + 287 + 157 = 2201 asercji PASS; pełne
      `--all` 23 zestawy / 5376 asercji PASS; `git diff --check` PASS; wersja
      nadal 0.27.0, bez instalacji/commita/taga/ZIP/wydania.

## 0.28.0 — końcowy kandydat wariantu A

- [x] Zakres zamknięty: produkcyjne auto-recovery v2 wyłącznie dla
      kwalifikujących się lokalnych linear Execute/Undo/Redo.
- [x] Swap v2 generator/dispatch/allowlista, recovery cykli, równoważne
      gwarancje KIO i power-loss przeniesione do roadmapy po 0.28.0.
- [x] Ograniczenia procesów niekooperujących i TOCTOU opisane jawnie w
      `RELEASE_NOTES_0.28.0.md`.
- [x] Świeży build po zmianie wersji i dokumentów: PASS (2026-09-20).
- [x] Świeży focused: Batch Rename 1790 + panes 287 + actions 157 = 2234 PASS.
- [x] Świeża pełna regresja: 23 zestawy / 5409 asercji PASS.
- [x] `git diff --check` po zmianach: PASS.
- [ ] Końcowy ręczny odbiór GUI na izolowanych kopiach i osobnym
      `XDG_DATA_HOME`. Jeśli nie wykonano interakcji człowieka z GUI, pozostawić
      jako **UNVERIFIED**, nawet gdy automatyczne black-box testy przechodzą.
- [x] Archiwum źródłowe i suma SHA-256 przygotowane bez `.git`, buildów,
      cache ani danych użytkownika; nie instalowano, nie commitowano, nie
      tagowano i nie publikowano.

## 0.29.0 — release candidate inteligentnych nazw

- [x] Stage 1 lokalny: podstawowy odbiór ręczny potwierdzony przez użytkownika.
- [x] Stage 2 zdalnego KIO: podstawowy przepływ na jednorazowej lokalizacji
      potwierdzony przez użytkownika. Nie oznacza to testu awarii połączenia,
      wszystkich protokołów, całej rozszerzonej checklisty ani atomowej
      rezerwacji nazwy.
- [x] Granica bezpieczeństwa: dwa asynchroniczne listingi są fail-closed, a
      końcowe operacje nie używają `Overwrite`/`Resume`; ostateczna ochrona
      zależy od kontraktu no-overwrite konkretnego workera i protokołu.
- [x] Świeży build RC po bumpie wersji: PASS (2026-09-20).
- [x] Świeży focused: FileActions 176, routing paneli 289 i Batch Rename 1790
      asercji PASS.
- [x] Świeża pełna regresja: 23 zestawy / 5430 asercji PASS.
- [x] `git diff --check`, spójność wersji oraz czyste archiwum źródłowe + SHA256:
      `kio-thispc-0.29.0.tar.gz` i `kio-thispc-0.29.0.tar.gz.sha256`, bez `.git`,
      buildów, `__pycache__` i runtime’owych dzienników recovery.
- [ ] Finalny commit, tag `v0.29.0` i instalacja wyłącznie po osobnej zgodzie.

## 0.30.0 — release candidate grouping/view settings

- [x] Stage 1 per-folder Icons/List/Details/Compact persistence: manual PASS.
- [x] Stage 1b View menu organization, active-pane routing and existing toggles: manual PASS.
- [x] Stage 1c icon sizes 96/64/48/32 px, per-folder persistence and stable icon geometry: manual PASS.
- [x] Stage 1d Compact in primary/Split View with directional navigation and persistence: manual PASS.
- [x] Stage 2 Type: all four views, Split View, Search, header PPM, sorting and persistence: manual PASS.
- [x] Stage 2 Date: documented local-calendar buckets, all four views, Split View/Search,
      header PPM, sorting and persistence: manual PASS.
- [x] Stage 2 Size: byte buckets/folders/unknowns, all four views, Split View/Search,
      header PPM, sorting and persistence: manual PASS.
- [x] Selection acceptance: plain click leaves one selected item; Ctrl/Shift multi-select remains;
      stale categorized hover no longer looks like a second selection; short/expanded-name outline PASS.
- [x] No selection diagnostic logging remains in production source.
- [x] Fresh Release build after 0.30.0 version/doc bump: PASS.
- [x] Fresh focused `view_settings panes actions`: 226 + 376 + 176 = 778 asercji PASS.
- [x] Fresh full `tests/run-pane-actions.py --all`: 24 zestawy / 6278 asercji PASS.
- [x] `git diff --check`: PASS.
- [x] Version consistency audit: active release surfaces report 0.30.0; historical 0.29.0 docs stay historical; no `THISPC_SELECT`/`SELECTION_DEBUG` leftovers.
- [ ] Source archive `kio-thispc-0.30.0.tar.gz` + `.sha256` prepared without `.git`, build,
      `tests/__pycache__`, `.directory`, runtime recovery journals or unrelated local files.
- [ ] Final diff/status reviewed; commit, tag `v0.30.0` and installation only after separate user approval.
