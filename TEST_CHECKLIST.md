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
